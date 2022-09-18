#ifndef MAINDISPMANX_H
#define MAINDISPMANX_H
#include "main.h"

struct frame_element *dispmanx_new_element(int layer);
void dispmanx_free_element(struct frame_element *e);

void dispmanx_update_frame(struct frame_element *e, struct video_frame *frame, void *frame_data);
void dispmanx_show(void);
void dispmanx_init();
void dispmanx_close();
void dispmanx_set_pos(struct frame_element *e, int dx, int dy, int zoom);


#endif
