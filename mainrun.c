#include <sys/socket.h>
#include <unistd.h>
#include <stdio.h>
#include <stdlib.h>
enum rt_message_type {
    LOAD_GAME = 1, // + path
    START_GAME = 2,
    PAUSE_GAME = 3,
    LOAD_STATE = 3,
    SAVE_STATE = 4, // + save state path
    UNLOAD_GAME = 5,
    FF_GAME = 6,
    INPUT_DATA = 7, // + events uint32
    KEYBOARD_DATA = 7 // 320/8 = 40b or events uint32[]
    VIDEO_OUT = 8, // uint32[]
    AUDIO_OUT = 9
    OPTION_SET = 9, // char*[]
    VSYNC = 10,

    OPTION_DESC = 129,
};

// port devices = 16-128 (4-8)
// kb (1) (port 3b + input 6b)/(key 9b) = 10b (1024*2 = 2048)       
// rt message input event = port/kb (4 bit) + inputid (9bit) + value (16bit)
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
int main() {
    struct core_worker *c = core_start("atari800");
    core_message(c, 42);
    pause();
    return 0;
}
