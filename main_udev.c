
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <errno.h>
#include <fcntl.h>
#include <linux/input.h>
#include <poll.h>
#include <unistd.h>

#include <libudev.h>
#include <pthread.h>

// Closing evdev descriptors is slow
// https://gitlab.freedesktop.org/libinput/libinput/-/issues/509
// https://patchwork.kernel.org/project/linux-input/patch/20201227144302.9419-1-kl@kl.wtf/
void *async_close_worker(void *fd) {
  close((int)fd);
  printf("done close %d\n", (int)fd);
  return 0;
}
void async_close(int fd) {
  pthread_t thread;
  pthread_attr_t tattr;
  pthread_attr_init(&tattr);
  pthread_attr_setdetachstate(&tattr, PTHREAD_CREATE_DETACHED);
  pthread_attr_setstacksize(&tattr, 4096);
  pthread_create(&thread, &tattr, async_close_worker, (void *)fd);
}

#define is_bit_set(a, i) (((1UL << ((i) % 64)) & ((a)[(i) / 64])) != 0)
#define NBITS(x) (((x) + 63) / 64)

static int udev_open_joystick(const char *path) {
  uint64_t event_type_mask[NBITS(EV_MAX)] = {0};
  uint64_t key_event_mask[NBITS(KEY_MAX)] = {0};
  uint64_t abs_event_mask[NBITS(ABS_MAX)] = {0};
  int fd = open(path, O_RDWR | O_NONBLOCK);

  if (fd < 0)
    return -1;
  if ((ioctl(fd, EVIOCGBIT(0, sizeof(event_type_mask)), event_type_mask) >= 0) &&
      (ioctl(fd, EVIOCGBIT(EV_KEY, sizeof(key_event_mask)), key_event_mask) >= 0) &&
      (ioctl(fd, EVIOCGBIT(EV_ABS, sizeof(abs_event_mask)), abs_event_mask) >= 0)) {

    if (is_bit_set(event_type_mask, EV_KEY)) {
      // TODO: check minimum capabilities to classify as keyboard/mouse/gamepad
      // TODO: calculate values for abs rescaling
      ioctl(fd, EVIOCGRAB, 1);
      return fd;
    }
  }
  async_close(fd);
  return -1;
}

struct device_entry {
  int fd;
};

#define MAX_DEVICES 32
static struct udev *udev;
static struct udev_monitor *udev_monitor;
static struct device_entry devices[MAX_DEVICES];

static void input_handler_close(void) {
  if (udev_monitor)
    udev_monitor_unref(udev_monitor);
  udev_monitor = 0;

  if (udev)
    udev_unref(udev);
  udev = 0;
}

static void check_device(struct udev_device *dev) {
  const char *devnode = udev_device_get_devnode(dev);
  if (!devnode)
    return;
  int is_gamepad = 0, is_keyboard = 0, is_mouse = 0;
  const char *serial_id = udev_device_get_property_value(dev, "ID_SERIAL");
  const char *port_id = udev_device_get_property_value(dev, "ID_PATH");
  const char *is_gamepad_str = udev_device_get_property_value(dev, "ID_INPUT_JOYSTICK");
  const char *is_keyboard_str = udev_device_get_property_value(dev, "ID_INPUT_KEYBOARD");
  const char *is_mouse_str = udev_device_get_property_value(dev, "ID_INPUT_MOUSE");
  if (is_gamepad_str)
    is_gamepad = atoi(is_gamepad_str);
  if (is_keyboard_str)
    is_keyboard = atoi(is_keyboard_str);
  if (is_mouse_str)
    is_mouse = atoi(is_mouse_str);
  int fd = udev_open_joystick(devnode);
  if (fd >= 0) {
    char name[128] = {0};
    ioctl(fd, EVIOCGNAME(sizeof(name) - 1), name);
    printf("%d%d%d: %s/%s/%s @ %s\n", is_gamepad, is_keyboard, is_mouse, name, port_id, serial_id, devnode);
    for (int i = 0; i < MAX_DEVICES; i++) {
      struct device_entry *d = devices + i;
      if (d->fd >= 0)
        continue;
      d->fd = fd;
      break;
    }
  }
}

static void check_new_devices() {
  struct pollfd fds = {
    fd : udev_monitor_get_fd(udev_monitor),
    events : POLLIN,
  };
  while (poll(&fds, 1, 0) > 0) {
    struct udev_device *dev = udev_monitor_receive_device(udev_monitor);
    if (dev) {
      if (!strcmp(udev_device_get_action(dev), "add"))
        check_device(dev);
      udev_device_unref(dev);
    }
  }
}

static void poll_devices(void) {
  if (udev_monitor)
    check_new_devices();

  for (int p = 0; p < MAX_DEVICES; p++) {
    int len;
    struct input_event events;
    struct device_entry *input = &devices[p];

    if (input->fd < 0)
      continue;

    while ((len = read(input->fd, &events, sizeof(events))) == sizeof(events)) {
      uint16_t type = events.type;
      uint16_t code = events.code;
      int32_t value = events.value;
      printf("%d %d %d\n", type, code, value);
    }
    if (len < 0 && errno != EAGAIN) {
      printf("%d %d\n", len, errno);
      if (errno == ENODEV) {
        async_close(input->fd);
        input->fd = -1;
      }
    }
  }
}
void input_handler_init(void *data) {
  for (int p = 0; p < MAX_DEVICES; p++) {
    devices[p].fd = -1;
  }
  udev = udev_new();
  if (!udev)
    return;
  udev_monitor = udev_monitor_new_from_netlink(udev, "udev");
  if (udev_monitor) {
    udev_monitor_filter_add_match_subsystem_devtype(udev_monitor, "input", NULL);
    udev_monitor_enable_receiving(udev_monitor);
  }
  struct udev_enumerate *enumerate = udev_enumerate_new(udev);
  if (!enumerate)
    goto error;

  udev_enumerate_add_match_property(enumerate, "ID_INPUT", "1");
  udev_enumerate_add_match_subsystem(enumerate, "input");
  udev_enumerate_scan_devices(enumerate);
  struct udev_list_entry *item, *devs = udev_enumerate_get_list_entry(enumerate);
  if (!devs)
    printf("no input devices\n");

  udev_list_entry_foreach(item, devs) {
    struct udev_device *dev = udev_device_new_from_syspath(udev, udev_list_entry_get_name(item));
    check_device(dev);
    udev_device_unref(dev);
  }

  udev_enumerate_unref(enumerate);

  for (;;) {
    poll_devices();
    usleep(1000000 / 60);
  }

error:
  input_handler_close();
}
