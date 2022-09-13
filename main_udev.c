
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

#include "libretro.h"
#include "main.h"
#include "mainlog.h"

// Closing evdev descriptors is slow
// https://gitlab.freedesktop.org/libinput/libinput/-/issues/509
// https://patchwork.kernel.org/project/linux-input/patch/20201227144302.9419-1-kl@kl.wtf/
void *async_close_worker(void *fd) {
  close((int)fd);
  rt_log("done close %d\n", (int)fd);
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

enum device_type {
  INPUT_DEVICE_KEYBOARD = 0,
  INPUT_DEVICE_GAMEPAD = 1,
  INPUT_DEVICE_MOUSE = 2,
};

#define BUTTON_MAP_MAX 32
#define AXIS_MAP_MAX 16
#define T_RETRO_AXIS_X 16
#define T_RETRO_AXIS_Y 17
#define T_RETRO_AXIS_RX 18
#define T_RETRO_AXIS_RY 19
#define T_RETRO_AXIS_NEG_X 20
#define T_RETRO_AXIS_NEG_Y 21
#define T_RETRO_AXIS_NEG_RX 22
#define T_RETRO_AXIS_NEG_RY 23
#define T_RETRO_HOME_KEY 24
#define T_RETRO_NONE 31
#define IS_BUTTON_MAP(x) ((x) < 16)
#define IS_AXIS_MAP(x) (((x)&24) == 16)
#define IS_AXIS_NEG_MAP(x) (((x)&28) == 20)
static const char *control_names[32] = {
    "B",  "Y",  "SELECT", "START", "UP", "DOWN", "LEFT", "RIGHT", "A",    "X", "L", "R", "L2", "R2", "L3", "R3",
    "+X", "+Y", "+RX",    "+RY",   "-X", "-Y",   "-RX",  "-RY",   "HOME", 0,   0,   0,   0,    0,    0,    "NONE",
};
#define DIGITAL_DEAD_ZONE 2000

uint8_t keyboardstate[(RETROK_LAST + 7) >> 3];
int16_t joy_state[CONTROLS_MAX];
int mode_state;
extern int keymap[KEY_MAX];

struct device_mapping {
  uint8_t button_map_len;
  uint8_t axis_map_len;
  struct button_mapping {
    uint16_t ev_button : 10;
    uint16_t retro_button : 5;
  } button_map[BUTTON_MAP_MAX];
  struct axis_mapping {
    uint16_t ev_axis : 6;
    uint16_t retro_button_pos : 5;
    uint16_t retro_button_neg : 5;
  } axis_map[AXIS_MAP_MAX];
};

struct device_mapping default_mapping = {25,
                                         8,
                                         {
                                             {BTN_A, RETRO_DEVICE_ID_JOYPAD_A},
                                             {BTN_Y, RETRO_DEVICE_ID_JOYPAD_Y},
                                             {BTN_B, RETRO_DEVICE_ID_JOYPAD_B},
                                             {BTN_X, RETRO_DEVICE_ID_JOYPAD_X},
                                             {BTN_SELECT, RETRO_DEVICE_ID_JOYPAD_SELECT},
                                             {BTN_START, RETRO_DEVICE_ID_JOYPAD_START},
                                             {BTN_TL, RETRO_DEVICE_ID_JOYPAD_L},
                                             {BTN_TR, RETRO_DEVICE_ID_JOYPAD_R},
                                             {BTN_TL2, RETRO_DEVICE_ID_JOYPAD_L2},
                                             {BTN_TR2, RETRO_DEVICE_ID_JOYPAD_R2},
                                             {BTN_THUMBL, RETRO_DEVICE_ID_JOYPAD_L3},
                                             {BTN_THUMBR, RETRO_DEVICE_ID_JOYPAD_R3},
                                             {BTN_DPAD_DOWN, RETRO_DEVICE_ID_JOYPAD_DOWN},
                                             {BTN_DPAD_UP, RETRO_DEVICE_ID_JOYPAD_UP},
                                             {BTN_DPAD_RIGHT, RETRO_DEVICE_ID_JOYPAD_RIGHT},
                                             {BTN_DPAD_LEFT, RETRO_DEVICE_ID_JOYPAD_LEFT},
                                             {BTN_MODE, T_RETRO_HOME_KEY},

                                             {BTN_C, T_RETRO_HOME_KEY},
                                             {BTN_Z, T_RETRO_HOME_KEY},
                                             // Joystick style controller mapping
                                             {BTN_TRIGGER, RETRO_DEVICE_ID_JOYPAD_A},
                                             {BTN_THUMB, RETRO_DEVICE_ID_JOYPAD_B},
                                             {BTN_THUMB2, RETRO_DEVICE_ID_JOYPAD_X},
                                             {BTN_TOP, RETRO_DEVICE_ID_JOYPAD_Y},
                                             {BTN_BASE, RETRO_DEVICE_ID_JOYPAD_SELECT},
                                             {BTN_BASE2, RETRO_DEVICE_ID_JOYPAD_START},
                                         },
                                         {
                                             {ABS_HAT0X, RETRO_DEVICE_ID_JOYPAD_RIGHT, RETRO_DEVICE_ID_JOYPAD_LEFT},
                                             {ABS_HAT0Y, RETRO_DEVICE_ID_JOYPAD_DOWN, RETRO_DEVICE_ID_JOYPAD_UP},
                                             {ABS_X, T_RETRO_AXIS_X, T_RETRO_NONE},
                                             {ABS_Y, T_RETRO_AXIS_Y, T_RETRO_NONE},
                                             {ABS_RX, T_RETRO_AXIS_RX, T_RETRO_NONE},
                                             {ABS_RY, T_RETRO_AXIS_RY, T_RETRO_NONE},
                                             {ABS_Z, RETRO_DEVICE_ID_JOYPAD_L2, T_RETRO_NONE},
                                             {ABS_RZ, RETRO_DEVICE_ID_JOYPAD_R2, T_RETRO_NONE},
                                         }};

struct device_entry {
  int fd;
  int port;
  enum device_type type;
  char short_name[128];
  char full_name[256];
  char bluetooth_addr[18];
  struct device_mapping mapping;
  uint32_t abs_range[ABS_MAX][2];
  union {
    int16_t gamepad_state[CONTROLS_MAX];
    uint8_t keyboard_state[(RETROK_LAST + 7) >> 3];
  } input_state;
  int home_state;
};

#define MAX_DEVICES 32
static struct udev *udev;
static struct udev_monitor *udev_monitor;
static struct device_entry devices[MAX_DEVICES];
static int num_devices = 0;

void input_handler_disconnect_bt(void) {
  for (int i = 0; i < num_devices; i++) {
    if (strlen(devices[i].bluetooth_addr)) {
      if (!fork()) {
        close(1);
        close(2);
        execlp("bluetoothctl", "bluetoothctl", "disconnect", devices[i].bluetooth_addr, NULL);
        exit(-1);
      }
    }
  }
}

void input_handler_close(void) {
  if (udev_monitor)
    udev_monitor_unref(udev_monitor);
  udev_monitor = 0;

  if (udev)
    udev_unref(udev);
  udev = 0;
}

static void check_device(struct udev_device *dev) {
  const char *devnode = udev_device_get_devnode(dev);
  if (!devnode || num_devices == MAX_DEVICES)
    return;
  int is_gamepad = 0, is_keyboard = 0, is_mouse = 0;
  // const char *serial_id = udev_device_get_property_value(dev, "ID_SERIAL");
  const char *port_id = udev_device_get_property_value(dev, "ID_PATH");
  const char *is_gamepad_str = udev_device_get_property_value(dev, "ID_INPUT_JOYSTICK");
  const char *is_keyboard_str = udev_device_get_property_value(dev, "ID_INPUT_KEYBOARD");
  const char *is_mouse_str = udev_device_get_property_value(dev, "ID_INPUT_MOUSE");
  const char *bus_id = udev_device_get_property_value(dev, "ID_BUS");
  int is_bluetooth = bus_id && !strcmp(bus_id, "bluetooth");
  if (is_gamepad_str)
    is_gamepad = atoi(is_gamepad_str);
  if (is_keyboard_str)
    is_keyboard = atoi(is_keyboard_str);
  if (is_mouse_str)
    is_mouse = atoi(is_mouse_str);

  uint64_t event_type_mask[NBITS(EV_MAX)] = {0};
  uint64_t key_event_mask[NBITS(KEY_MAX)] = {0};
  uint64_t abs_event_mask[NBITS(ABS_MAX)] = {0};
  int fd = open(devnode, O_RDWR | O_NONBLOCK | O_CLOEXEC);

  if (fd < 0)
    return;
  ioctl(fd, EVIOCGBIT(0, sizeof(event_type_mask)), event_type_mask);
  ioctl(fd, EVIOCGBIT(EV_KEY, sizeof(key_event_mask)), key_event_mask);
  ioctl(fd, EVIOCGBIT(EV_ABS, sizeof(abs_event_mask)), abs_event_mask);

  if (!is_bit_set(event_type_mask, EV_KEY) || !(is_gamepad || is_keyboard || is_mouse)) {
    // TODO: check minimum capabilities to classify as keyboard/mouse/gamepad
    async_close(fd);
    return;
  }

  struct device_entry *d = devices + num_devices++;
  memset(d, 0, sizeof(*d));

  if (is_keyboard)
    ioctl(fd, EVIOCGRAB, 1);

  char name[128] = {0};
  ioctl(fd, EVIOCGNAME(sizeof(name) - 1), name);
  struct input_id inputid = {0};
  ioctl(fd, EVIOCGID, &inputid);
  const char *bluetooth_mac = 0;
  d->bluetooth_addr[0] = 0;
  if (is_bluetooth) {
    struct udev_device *parent = udev_device_get_parent(dev);
    if (parent) {
      bluetooth_mac = udev_device_get_property_value(parent, "UNIQ");
      if (bluetooth_mac && strlen(bluetooth_mac) == 19) {
        strncpy(d->bluetooth_addr, bluetooth_mac + 1, 17);
      }
    }
  }
  rt_log("%04x:%04x:%s/%s/%s\n", inputid.vendor, inputid.product, name, port_id, bluetooth_mac);
  d->fd = fd;
  d->port = 0;
  if (is_gamepad)
    d->type = INPUT_DEVICE_GAMEPAD;
  else if (is_mouse)
    d->type = INPUT_DEVICE_MOUSE;
  else
    d->type = INPUT_DEVICE_KEYBOARD;

  for (int i = 0; i < ABS_MAX; i++) {
    if (is_bit_set(abs_event_mask, i)) {
      struct input_absinfo absinfo;
      if (ioctl(fd, EVIOCGABS(i), &absinfo) < 0)
        continue;
      d->abs_range[i][0] = absinfo.minimum;
      d->abs_range[i][1] = absinfo.maximum - absinfo.minimum;
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

static struct button_mapping *get_button_mapping(int key) {
  for (int i = 0; i < default_mapping.button_map_len; i++) {
    if (default_mapping.button_map[i].ev_button == key)
      return &default_mapping.button_map[i];
  }
  return 0;
}
static struct axis_mapping *get_axis_mapping(int axis) {
  for (int i = 0; i < default_mapping.axis_map_len; i++) {
    if (default_mapping.axis_map[i].ev_axis == axis)
      return &default_mapping.axis_map[i];
  }
  return 0;
}

int map_axis_value(struct device_entry *input, int axis, int value) {
  int min = input->abs_range[axis][0];
  int range = input->abs_range[axis][1];
  if (range <= 0)
    return 0;
  value -= min;
  if (range % 2 && range > 1 && min == 0) {
    // odd range hack - map the two central values to 0 and make the range smaller by 1
    if (value > range / 2)
      value -= 1;
    range -= 1;
  }
  return (int64_t)value * 0xfffe / range - 0x7fff;
}

void set_button_value(int16_t *gamepad_state, int button, int value) {
  if (value)
    gamepad_state[0] |= 1 << button;
  else
    gamepad_state[0] &= ~(1 << button);
}
void set_key_value(uint8_t *keyboard_state, int id, int v) {
  if (v)
    keyboard_state[id >> 3] |= 1 << (id & 7);
  else
    keyboard_state[id >> 3] &= ~(1 << (id & 7));
}

uint8_t *get_keyboard_state() {
  uint8_t *res = 0;
  for (int p = 0; p < num_devices; p++) {
    if (devices[p].type == INPUT_DEVICE_KEYBOARD) {
      if (!res)
        res = devices[p].input_state.keyboard_state;
      else {
        if (res != keyboardstate) {
          memcpy(keyboardstate, res, sizeof(keyboardstate));
          res = keyboardstate;
        }
        for (int i = 0; i < sizeof(keyboardstate); i++) {
          keyboardstate[i] |= devices[p].input_state.keyboard_state[i];
        }
      }
    }
  }
  if (!res) {
    memset(keyboardstate, 0, sizeof(keyboardstate));
    res = keyboardstate;
  }
  return res;
}

int16_t *get_gamepad_state(int port) {
  int16_t *res = 0;
  for (int p = 0; p < num_devices; p++) {
    if (devices[p].type == INPUT_DEVICE_GAMEPAD && (port < 0 || devices[p].port == port)) {
      if (!res)
        res = devices[p].input_state.gamepad_state;
      else {
        if (res != joy_state) {
          memcpy(joy_state, res, sizeof(joy_state));
          res = joy_state;
        }
        joy_state[0] |= devices[p].input_state.gamepad_state[0];
        for (int i = 1; i < CONTROLS_MAX; i++) {
          if (abs(devices[p].input_state.gamepad_state[i]) > abs(joy_state[i]))
            joy_state[i] = devices[p].input_state.gamepad_state[i];
        }
      }
    }
  }
  if (!res) {
    memset(joy_state, 0, sizeof(joy_state));
    res = joy_state;
  }
  return res;
}

uint32_t poll_devices(void) {
  int ports_dirty = 0, keyboard_dirty = 0, home_state = 0;
  if (udev_monitor)
    check_new_devices();

  for (int p = 0; p < num_devices; p++) {
    int len;
    struct input_event ev;
    struct device_entry *input = &devices[p];

    if (input->fd < 0)
      continue;

    while ((len = read(input->fd, &ev, sizeof(ev))) == sizeof(ev)) {
      if (input->type == INPUT_DEVICE_GAMEPAD) {
        if (ev.type == EV_KEY) {
          struct button_mapping *m = get_button_mapping(ev.code);
          rt_log("EV_KEY %d -> %s = %d\n", ev.code, m ? control_names[m->retro_button] : "undefined", ev.value);
          if (m && IS_BUTTON_MAP(m->retro_button)) {
            set_button_value(input->input_state.gamepad_state, m->retro_button, ev.value);
            if (input->port >= 0)
              ports_dirty |= 1 << input->port;
          }
          if (m && m->retro_button == T_RETRO_HOME_KEY) {
            input->home_state = ev.value;
          }
        } else if (ev.type == EV_ABS) {
          struct axis_mapping *m = get_axis_mapping(ev.code);
          int v = map_axis_value(input, ev.code, ev.value);
          // printf("EV_ABS %d -> (%s/%s) = %d -> %d\n", ev.code, m ? control_names[m->retro_button_pos] : "undefined",
          //        m ? control_names[m->retro_button_neg] : "undefined", ev.value, v);
          if (m) {
            if (input->port >= 0)
              ports_dirty |= 1 << input->port;
            if (IS_BUTTON_MAP(m->retro_button_pos)) {
              int digital_value = v > (m->retro_button_neg == T_RETRO_NONE ? -0x7fff : 0) + DIGITAL_DEAD_ZONE;
              set_button_value(input->input_state.gamepad_state, m->retro_button_pos, digital_value);
            }
            if (IS_BUTTON_MAP(m->retro_button_neg)) {
              int digital_value = v < (m->retro_button_pos == T_RETRO_NONE ? 0x7fff : 0) - DIGITAL_DEAD_ZONE;
              set_button_value(input->input_state.gamepad_state, m->retro_button_neg, digital_value);
            }
            if (IS_AXIS_MAP(m->retro_button_pos)) {
              int retro_axis_id = m->retro_button_pos & 3;
              int analog_value = IS_AXIS_NEG_MAP(m->retro_button_pos) ? -v : v;
              input->input_state.gamepad_state[retro_axis_id + 1] = analog_value;
            }
          }
        }
      }
      if (input->type == INPUT_DEVICE_KEYBOARD && ev.type == EV_KEY && ev.value < 2) {
        rt_log("EV_KEY %d %d\n", ev.code, ev.value);
        set_key_value(input->input_state.keyboard_state, keymap[ev.code], ev.value);
        keyboard_dirty = 1;
      }
    }
    home_state |= input->home_state;
    if (len < 0 && errno != EAGAIN) {
      rt_log("%d %d\n", len, errno);
      if (errno == ENODEV) {
        async_close(input->fd);
        *input = devices[--num_devices];
        p--;
      }
    }
  }
  return (home_state << (MAX_PORTS + 1)) + (keyboard_dirty << MAX_PORTS) + ports_dirty;
}
void input_handler_init(void) {
  udev = udev_new();
  if (!udev)
    return;
  udev_monitor = udev_monitor_new_from_netlink(udev, "udev");
  if (udev_monitor) {
    udev_monitor_filter_add_match_subsystem_devtype(udev_monitor, "input", NULL);
    udev_monitor_enable_receiving(udev_monitor);
  }
  struct udev_enumerate *enumerate = udev_enumerate_new(udev);
  if (!enumerate) {
    input_handler_close();
    return;
  }

  udev_enumerate_add_match_property(enumerate, "ID_INPUT", "1");
  udev_enumerate_add_match_subsystem(enumerate, "input");
  rt_log("enumarating input devices\n");
  udev_enumerate_scan_devices(enumerate);
  struct udev_list_entry *item, *devs = udev_enumerate_get_list_entry(enumerate);

  udev_list_entry_foreach(item, devs) {
    struct udev_device *dev = udev_device_new_from_syspath(udev, udev_list_entry_get_name(item));
    check_device(dev);
    udev_device_unref(dev);
  }

  udev_enumerate_unref(enumerate);
}
