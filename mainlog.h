#include "libretro.h"

int64_t timestamp();
void rt_log_init(char c);
void rt_log(const char *fmt, ...);
void rt_log_v(const char *fmt, enum retro_log_level level, va_list args);
