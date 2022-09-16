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

struct core_worker {
    int comm_socket;
    int worker_pid;
    int last_id;
    struct shared_memory *memory;
};

#define acquire(m) while (atomic_flag_test_and_set(m))
#define release(m) atomic_flag_clear(m)

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
    worker->last_id = 0;
    return worker;
}
void core_message(struct core_worker *core, int message) {
    write(core->comm_socket, &message, sizeof(message));
}
void core_message_keyboard_data(struct core_worker *core, uint8_t *data) {
    if (data) memcpy(core->memory->keyboard_state, data, sizeof(core->memory->keyboard_state));
    else memset(core->memory->keyboard_state, 0, sizeof(core->memory->keyboard_state));
}
void core_message_input_data(struct core_worker *core, int port, int16_t *data) {
    if (data) memcpy(core->memory->joypad_state[port], data, sizeof(core->memory->joypad_state[port]));
    else memset(core->memory->joypad_state[port], 0, sizeof(core->memory->joypad_state[port]));
}
void core_stop(struct core_worker *core) {
    core_message(core, QUIT_CORE);
    if (core->memory) munmap(core->memory, SHARED_MEM_SIZE);
    if (core->comm_socket >= 0) close(core->comm_socket);
    free(core);
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
    struct core_worker *worker;
    int gfx_id;
} list[MAX_CONTENT];
int load_content_queue() {
    char line[FILENAME_MAX * 2];
    int i = 0;
    FILE *f = fopen(CONTENT_QUEUE_FILE, "r");
    if (!f) return 0;
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
    return i;
}
static int done = 0;
void term(int signum) {
    rt_log("term signal\n");
    done = 1;
}
int current_content_idx = 0;
int8_t gfxres[16];
int alloc_gfx_id() {
    for (int i = 0; i < sizeof(gfxres); i++) {
        if (!gfxres[i]) {
            gfxres[i] = 1;
            return i;
        }
    }
    return 0;
}
void free_gfx_id(int id) {
    if (id) gfxres[id] = 0;
}
void update_workers() {
    for (int i = 0; i < MAX_CONTENT && list[i].core; i++) {
        int diff = i - current_content_idx;
        if (diff >= -1 && diff <= 3) {
            if (!list[i].worker) {
                list[i].worker = core_start(list[i].core, list[i].filename);
                list[i].gfx_id = alloc_gfx_id();
                dispmanx_set_pos(list[i].gfx_id, -180 + diff * 250, 0, diff ? 0x80 : 0xC0);
            }
        } else {
            if (list[i].worker) {
                free_gfx_id(list[i].gfx_id);
                dispmanx_set_pos(list[i].gfx_id, 800, 0, 0x200);
                core_stop(list[i].worker);
                list[i].worker = 0;
                list[i].gfx_id = 0;
            }
        }
    }
}
int main() {
    rt_log_init(' ');
    signal(SIGTERM, term);
    signal(SIGINT, term);
    input_handler_init();
    dispmanx_init();
    rt_log("udev input initialized\n");
    int num_content = load_content_queue();
    update_workers();
    rt_log("cores spawned\n");
    core_message(list[0].worker, START_GAME);
    for (;!done;) {
        int r = poll_devices();
        int ui_controls = get_ui_controls(r);
//        if (ui_controls) printf("%04x\n", ui_controls);
        if (ui_controls & ((1 << RETRO_DEVICE_ID_JOYPAD_LEFT) | (1 << RETRO_DEVICE_ID_JOYPAD_RIGHT))) {
            current_content_idx = ui_controls & (1 << RETRO_DEVICE_ID_JOYPAD_LEFT) ? current_content_idx + num_content - 1 : current_content_idx + 1;
            current_content_idx %= num_content;
            update_workers();
            for (int i = 0; i < num_content; i++) if (list[i].worker) {
                core_message(list[i].worker, i == current_content_idx ? START_GAME : PAUSE_GAME);
                dispmanx_set_pos(list[i].gfx_id, -180 + (i - current_content_idx) * 250, 0, i == current_content_idx ? 0xC0 : 0x80);
            }
        }
        if (ui_controls & (1 << RETRO_DEVICE_ID_JOYPAD_A)) {
            ui_controls |= UI_FOCUS_CHANGE;
            ui_focus = 0;
        }
        if (ui_controls & UI_FOCUS_CHANGE) {
            if (!ui_focus) {
                for (int i = 0; i < num_content; i++) if (list[i].worker) {
                    core_input_focus(list[i].worker, i == current_content_idx);
                    dispmanx_set_pos(list[i].gfx_id, i == current_content_idx ? 0 : 800, 0, 0x200);
                }
            } else {
                for (int i = 0; i < num_content; i++) if (list[i].worker) {
                    core_input_focus(list[i].worker, 0);
                    dispmanx_set_pos(list[i].gfx_id, -180 + (i - current_content_idx) * 250, 0, i == current_content_idx ? 0xC0 : 0x80);
                }
            }
        }
        if ((r & 31) && !ui_focus) {
            struct core_worker *c_focus = list[current_content_idx].worker;
            if (r&1 && c_focus) core_message_input_data(c_focus, 0, get_gamepad_state(0));
            if (r&2 && c_focus) core_message_input_data(c_focus, 1, get_gamepad_state(1));
            if (r&4 && c_focus) core_message_input_data(c_focus, 2, get_gamepad_state(2));
            if (r&8 && c_focus) core_message_input_data(c_focus, 3, get_gamepad_state(3));
            if (r&16 && c_focus) core_message_keyboard_data(c_focus, get_keyboard_state());
        }
        for (int i = 0; i < num_content; i++) if (list[i].worker && list[i].worker->memory) {
            struct core_worker *worker = list[i].worker;
            struct video_frame *last_frame = &list[i].worker->memory->frame;
            last_frame->data = &list[i].worker->memory->frame_data;
            acquire(&last_frame->mutex);
            if (last_frame->id == 0) {
            //    rt_log("frame dupped\n");
            } else {
                dispmanx_update_frame(list[i].gfx_id, last_frame);
                if (worker->last_id + 1 != last_frame->id) {
                    rt_log("%d frame(s) dropped\n", last_frame->id - worker->last_id - 1);
                }
                worker->last_id = last_frame->id;
                last_frame->id = 0;
            }
            release(&last_frame->mutex);
        }
        dispmanx_show();
    }
    for (int i = 0; i < num_content; i++) if (list[i].worker) {
        core_message(list[i].worker, QUIT_CORE);
    }
    input_handler_disconnect_bt();
    dispmanx_close();
    return 0;
}
