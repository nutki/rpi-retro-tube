#include <sys/socket.h>
#include <unistd.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <signal.h>
#include <sys/mman.h>
#include <fcntl.h>
#include "maininput.h"
#include "main.h"
#include "mainlog.h"
#include "maindispmanx.h"
#include "mainsdtvmode.h"

#define SHARED_MEM_SIZE (4 * 1024 * 1024)
struct core_worker {
    int comm_socket;
    int worker_pid;
    char *memory;
};



struct core_worker *core_start(char *id, char *content) {
    int s[2];
    if (socketpair(AF_LOCAL, SOCK_DGRAM, 0, s) < 0) {
        perror("socketpair");
        return NULL;
    }
    int m = memfd_create("mmap", 0);
    ftruncate(m, SHARED_MEM_SIZE);
    void *mem = mmap(0, SHARED_MEM_SIZE, PROT_READ | PROT_WRITE, MAP_SHARED, m, 0);
    if (!mem) {
        perror("mmap");
        return NULL;
    }
    int pid = fork();
    if (pid < 0) {
        perror("fork");
        return NULL;
    }
    if (!pid) {
        char sockno[20], memno[20];
        sprintf(sockno, "S=%d", s[1]);
        sprintf(memno, "M=%d", m);
        char * env[] = { sockno, memno, NULL };
        char * arg[] = { "main", id, content, NULL };
        execve("./main", arg, env);
        perror("exec");
        exit(-1);
    }
    close(m);
    close(s[1]);
    struct core_worker *worker = malloc(sizeof(struct core_worker));
    if (!worker) return NULL;
    worker->comm_socket = s[0];
    worker->worker_pid = pid;
    worker->memory = mem;
    strcpy(mem, "TEEEEST!");
    return worker;
}
void core_message(struct core_worker *core, int message) {
    write(core->comm_socket, &message, sizeof(message));
}
void core_message_keyboard_data(struct core_worker *core, uint8_t *data) {
    struct message_keyboard_data message = { KEYBOARD_DATA };
    if (data) memcpy(message.data, data, sizeof(message.data));
    else memset(message.data, 0, sizeof(message.data));
    write(core->comm_socket, &message, sizeof(message));
}
void core_message_input_data(struct core_worker *core, int port, int16_t *data) {
    struct message_input_data message = { INPUT_DATA };
    message.port = port;
    if (data) memcpy(message.data, data, sizeof(message.data));
    else memset(message.data, 0, sizeof(message.data));
    write(core->comm_socket, &message, sizeof(message));
}
void core_input_focus(struct core_worker *core, int on) {
    core_message_input_data(core, 0, on ? get_gamepad_state(0) : 0);
    core_message_input_data(core, 1, on ? get_gamepad_state(1) : 0);
    core_message_input_data(core, 2, on ? get_gamepad_state(2) : 0);
    core_message_input_data(core, 3, on ? get_gamepad_state(3) : 0);
    core_message_keyboard_data(core, on ? get_keyboard_state() : 0);
}
static int ui_focus = 1;
#define UI_FOCUS_CHANGE (1<<16)
int get_ui_controls(int r) {
    static int last_home_state = 0;
    static int last_controls_state = 0;
    static int autorepeat_clock = 0;
    int home_state = r & 32 ? 1 : 0;
    int controls_state = ui_focus ? get_gamepad_state(-1)[0] : 0;
    int ui_focus_change = 0;
    if (home_state && !last_home_state) {
        ui_focus = !ui_focus;
        last_controls_state = 0;
        controls_state = 0;
        ui_focus_change = UI_FOCUS_CHANGE;
    }
    autorepeat_clock = controls_state && controls_state == last_controls_state ? autorepeat_clock + 1 : 0;
    int pressed_buttons = controls_state & ~last_controls_state;
    if (autorepeat_clock >= 30 && (autorepeat_clock - 30) % 6 == 0) pressed_buttons = controls_state;
    last_controls_state = controls_state;
    last_home_state = home_state;
    return pressed_buttons | ui_focus_change;
}
#define MAX_CONTENT 256
#define CONTENT_QUEUE_FILE "./.rt.queue"
struct content_list {
    char *core, *filename;
} list[MAX_CONTENT];
void load_content_queue() {
    char line[FILENAME_MAX * 2];
    int i = 0;
    FILE *f = fopen(CONTENT_QUEUE_FILE, "r");
    if (!f) return;
    while(fgets(line, sizeof(line), f)) {
        int len = strlen(line);
        if (line[len - 1] == '\n') line[--len] = 0;
        if (!len || *line == '#') continue;
        char *s = strchr(line, ' ');
        if (s) *s++ = 0;
        list[i].core = strdup(line);
        list[i].filename = s ? strdup(s) : 0;
        printf("<%s> <%s>\n", line, s);
        i++;
    }
}
struct core_worker *workers[MAX_CONTENT];
static int done = 0;
static int num_workers = 0;
void term(int signum) {
    rt_log("term signal\n");
    done = 1;
}
int main() {
    rt_log_init(' ');
    signal(SIGTERM, term);
    signal(SIGINT, term);
    input_handler_init();
    dispmanx_init();
    rt_log("udev input initialized\n");
    int current_worker_idx = 0;
    load_content_queue();
    for (int i = 0; i < MAX_CONTENT && list[i].core; i++) {
        num_workers++;
        workers[i] = core_start(list[i].core, list[i].filename);
        dispmanx_set_pos(i, -180 + i * 250, 0, i ? 0x80 : 0xC0);
    }
    rt_log("cores spawned\n");
    core_message(workers[0], START_GAME);
    for (;!done;) {
        int r = poll_devices();
        int ui_controls = get_ui_controls(r);
//        if (ui_controls) printf("%04x\n", ui_controls);
        if (ui_controls & ((1 << RETRO_DEVICE_ID_JOYPAD_LEFT) | (1 << RETRO_DEVICE_ID_JOYPAD_RIGHT))) {
            current_worker_idx = ui_controls & (1 << RETRO_DEVICE_ID_JOYPAD_LEFT) ? current_worker_idx + num_workers - 1 : current_worker_idx + 1;
            current_worker_idx %= num_workers;
            for (int i = 0; i < num_workers; i++) if (workers[i]) {
                core_message(workers[i], i == current_worker_idx ? START_GAME : PAUSE_GAME);
                dispmanx_set_pos(i, -180 + (i - current_worker_idx) * 250, 0, i == current_worker_idx ? 0xC0 : 0x80);
            }
        }
        if (ui_controls & (1 << RETRO_DEVICE_ID_JOYPAD_A)) {
            ui_controls |= UI_FOCUS_CHANGE;
            ui_focus = 0;
        }
        if (ui_controls & UI_FOCUS_CHANGE) {
            if (!ui_focus) {
                for (int i = 0; i < num_workers; i++) if (workers[i]) {
                    core_input_focus(workers[i], i == current_worker_idx);
                    dispmanx_set_pos(i, i == current_worker_idx ? 0 : 800, 0, 0x200);
                }
            } else {
                for (int i = 0; i < num_workers; i++) if (workers[i]) {
                    core_input_focus(workers[i], 0);
                    dispmanx_set_pos(i, -180 + (i - current_worker_idx) * 250, 0, i == current_worker_idx ? 0xC0 : 0x80);
                }
            }
        }
        if ((r & 31) && !ui_focus) {
            struct core_worker *c_focus = workers[current_worker_idx];
            if (r&1 && c_focus) core_message_input_data(c_focus, 0, get_gamepad_state(0));
            if (r&2 && c_focus) core_message_input_data(c_focus, 1, get_gamepad_state(1));
            if (r&4 && c_focus) core_message_input_data(c_focus, 2, get_gamepad_state(2));
            if (r&8 && c_focus) core_message_input_data(c_focus, 3, get_gamepad_state(3));
            if (r&16 && c_focus) core_message_keyboard_data(c_focus, get_keyboard_state());
        }
        for (int i = 0; i < num_workers; i++) if (workers[i]->memory) {
            struct video_frame last_frame = *(struct video_frame *)(workers[i]->memory);
            last_frame.data = workers[i]->memory + 64;
            if (last_frame.fmt) dispmanx_update_frame(i, &last_frame);
        }
        usleep(1000000/60);
        dispmanx_show();
    }
    for (int i = 0; i < num_workers; i++) if (workers[i]) {
        core_message(workers[i], QUIT_CORE);
    }
    input_handler_disconnect_bt();
    dispmanx_close();
    return 0;
}
