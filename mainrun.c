#include <sys/socket.h>
#include <unistd.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "maininput.h"
#include "main.h"
struct core_worker {
    int comm_socket;
    int worker_pid;
};



struct core_worker *core_start(char *id) {
    int s[2];
    if (socketpair(AF_LOCAL, SOCK_DGRAM, 0, s) < 0) {
        perror("socketpair");
        return NULL;
    }
    int pid = fork();
    if (pid < 0) {
        perror("fork");
        return NULL;
    }
    if (!pid) {
        char sockno[20];
        sprintf(sockno, "S=%d", s[1]);
        char * env[] = { sockno, NULL };
        char * arg[] = { "main", id, NULL };
        execve("./main", arg, env);
        perror("exec");
        exit(-1);
    }
    struct core_worker *worker = malloc(sizeof(struct core_worker));
    if (!worker) return NULL;
    worker->comm_socket = s[0];
    worker->worker_pid = pid;
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
void core_message_video_out(struct core_worker *core, int x, int y, int zoom) {
    struct message_video_out message = { VIDEO_OUT };
    message.posx = x;
    message.posy = y;
    message.zoom = zoom;
    write(core->comm_socket, &message, sizeof(message));
}
void core_input_focus(struct core_worker *core, int on) {
    core_message_input_data(core, 0, on ? joy_state : 0);
    core_message_keyboard_data(core, on ? keyboardstate : 0);
}
int main() {
    extern void *input_handler_init(void);
    extern uint32_t poll_devices(void);
    extern int16_t *get_gamepad_state(int port);
    extern uint8_t *get_keyboard_state(void);
    input_handler_init();
//    init_input();
    struct core_worker *c = core_start("atari800");
    struct core_worker *c_focus = c;
    struct core_worker *c2 = core_start("cplus4");
    core_message_video_out(c, -180, 0, 0x100);
    core_message_video_out(c2, 180, 0, 0x100);
    core_message(c, 42);
    for (;;) {
        int r = poll_devices();
        if (r) {
            if (r&32) {
                printf("focus change\n");
                c_focus = c_focus == c ? c2 : c;
                core_input_focus(c, c_focus == c);
                core_input_focus(c2, c_focus == c2);
//                core_message_video_out(c, c_focus ? 0 : -180, 0, c_focus ? 0x200 : 0x100);
            }
            if (r&1 && c_focus) core_message_input_data(c_focus, 0, get_gamepad_state(0));
            if (r&2 && c_focus) core_message_input_data(c_focus, 1, get_gamepad_state(1));
            if (r&4 && c_focus) core_message_input_data(c_focus, 2, get_gamepad_state(2));
            if (r&8 && c_focus) core_message_input_data(c_focus, 3, get_gamepad_state(3));
            if (r&16 && c_focus) core_message_keyboard_data(c_focus, get_keyboard_state());
        }
        usleep(10000);
    }
    return 0;
}
