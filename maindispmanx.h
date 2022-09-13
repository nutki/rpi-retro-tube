#ifndef MAINDISPMANX_H
#define MAINDISPMANX_H
#include "main.h"

void dispmanx_update_frame(int idx, struct video_frame *frame);
void dispmanx_show(void);
void dispmanx_init();
void dispmanx_close();
void dispmanx_set_pos(int idx, int dx, int dy, int zoom);

#endif
