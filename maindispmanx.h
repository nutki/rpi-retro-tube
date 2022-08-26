#ifndef MAINDISPMANX_H
#define MAINDISPMANX_H

struct video_frame {
    int w, h, pitch, fmt;
    float aspect;
    void *data;
};

void dispmanx_show_frame(struct video_frame *frame);
void dispmanx_show_last(void);
void dispmanx_init();
void dispmanx_close();
void dispmanx_set_pos(int dx, int dy, int zoom);

#endif
