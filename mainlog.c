#include <stdint.h>
#include <stdio.h>
#include <stdarg.h>
#include <sys/time.h>
#include "libretro.h"
#include "mainlog.h"

static struct timeval logstart;
static char log_id = ' ';
int64_t timestamp() {
    struct timeval stop;
    gettimeofday(&stop, NULL);
    return (stop.tv_sec - logstart.tv_sec) * 1000000ll + stop.tv_usec - logstart.tv_usec;
}

void rt_log_init(char c) {
    log_id = c;
    gettimeofday(&logstart, NULL);
}

void rt_log(const char *fmt, ...) {
    va_list args;
    va_start(args, fmt);
    printf("%c%06llu ", log_id, timestamp());
    vprintf(fmt, args);
    va_end(args);
}

void rt_log_v(const char *fmt, enum retro_log_level level, va_list args) {
    if (level <= 0) return;
    printf("%c%06llu CORE %d: ", log_id, timestamp(), level);
    vprintf(fmt, args);
}
