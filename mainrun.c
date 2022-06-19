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
void core_message_keyboard_data(struct core_worker *core) {
    struct message_keyboard_data message = { KEYBOARD_DATA };
    memcpy(message.data, keyboardstate, sizeof(keyboardstate));
    write(core->comm_socket, &message, sizeof(message));
}
void core_message_input_data(struct core_worker *core) {
    struct message_input_data message = { INPUT_DATA };
    message.port = 0;
    memcpy(message.data, joy_state, sizeof(joy_state));
    write(core->comm_socket, &message, sizeof(message));
}
int main() {
    init_input();
    struct core_worker *c = core_start("atari800");
    core_message(c, 42);
    for (;;) {
        int r = poll_input();
        if (r) {
            core_message_input_data(c);
            core_message_keyboard_data(c);
        }
        usleep(10000);
    }
    return 0;
}
