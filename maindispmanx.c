#include "libretro.h"
#include "mainlog.h"
#include <assert.h>
#include "maindispmanx.h"
#include <bcm_host.h>
#include <pthread.h>
#include "mainsdtvmode.h"
#include "mainpal60.h"
#include <stdatomic.h>

#define ELEMENT_CHANGE_LAYER (1 << 0)
#define ELEMENT_CHANGE_OPACITY (1 << 1)
#define ELEMENT_CHANGE_DEST_RECT (1 << 2)
#define ELEMENT_CHANGE_SRC_RECT (1 << 3)
#define ELEMENT_CHANGE_MASK_RESOURCE (1 << 4)
#define ELEMENT_CHANGE_TRANSFORM (1 << 5)

#define MAX_ELEMENTS 16
static DISPMANX_DISPLAY_HANDLE_T display;
static struct frame_element {
  DISPMANX_RESOURCE_HANDLE_T element;
  DISPMANX_RESOURCE_HANDLE_T resource;
  DISPMANX_RESOURCE_HANDLE_T old_resource;
  int w, h, pitch, mode;
  float aspect;
  int x, y, zoom;
  int dst_rect_dirty, src_rect_dirty;
  int time_us;
  int framerate_dirty;
} frame_elements[MAX_ELEMENTS];

static int screenX, screenY, screenXoffset;

VC_IMAGE_TYPE_T pixel_format_to_mode(enum retro_pixel_format fmt) {
  switch (fmt) {
  case RETRO_PIXEL_FORMAT_XRGB8888:
    return VC_IMAGE_XRGB8888;
  case RETRO_PIXEL_FORMAT_RGB565:
    return VC_IMAGE_RGB565;
  case RETRO_PIXEL_FORMAT_UNKNOWN:
  case RETRO_PIXEL_FORMAT_0RGB1555:
    break;
  }
  rt_log("pixel format not supported: %d\n", fmt);
  exit(0);
  return 0;
}
int pixel_format_to_size(enum retro_pixel_format fmt) {
  switch (fmt) {
  case RETRO_PIXEL_FORMAT_XRGB8888:
    return 4;
  case RETRO_PIXEL_FORMAT_RGB565:
    return 2;
  case RETRO_PIXEL_FORMAT_UNKNOWN:
  case RETRO_PIXEL_FORMAT_0RGB1555:
    break;
  }
  rt_log("pixel format not supported: %d\n", fmt);
  exit(0);
  return 0;
}

void dispmanx_set_pos(int idx, int dx, int dy, int zoom) {
  struct frame_element *fe = &frame_elements[idx];
  fe->dst_rect_dirty |= fe->zoom != zoom || fe->x != dx || fe->y != dy;
  fe->zoom = zoom;
  fe->x = dx;
  fe->y = dy;
}
static int apply_zoom(struct frame_element *fe, int v) {
  int zoom = fe->zoom;
  if (fe->h > (fe->time_us < 19000 ? 240 : 288)) zoom /= 2;
  return (v * zoom) >> 8;
}
int needs_reinit = 0;
static pthread_mutex_t update_mutex = PTHREAD_MUTEX_INITIALIZER;
static pthread_mutex_t vsync_mutex = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t vsync_cond = PTHREAD_COND_INITIALIZER;
static int dispmanx_display_init(void);
int original_mode;
int pal60_hack;
void dispmanx_show() {
  // int has_dirty = 0;
  // for (int i = 0; i < MAX_ELEMENTS; i++) {
  //   struct frame_element *fe = &frame_elements[i];
  //   has_dirty = has_dirty || fe->src_rect_dirty || fe->dst_rect_dirty;
  // }
  // if (!has_dirty) return;
  pthread_mutex_lock(&update_mutex);
  if (needs_reinit) {
    int result;
    DISPMANX_UPDATE_HANDLE_T update = vc_dispmanx_update_start(0);
    for (int i = 0; i < MAX_ELEMENTS; i++) {
      if (frame_elements[i].element)
        result = vc_dispmanx_element_remove(update, frame_elements[i].element);
      frame_elements[i].element = 0;
    }
    result = vc_dispmanx_update_submit_sync(update);
    if (display) {
      vc_dispmanx_vsync_callback(display, NULL, NULL);
      result = vc_dispmanx_display_close(display);
      assert(result == 0);
    }
    dispmanx_display_init();
    rt_log("reinitialized\n");
  }
  
  struct frame_element *fullscreen_element = 0;
  static struct frame_element *prev_fullscreen_element = 0;
  DISPMANX_UPDATE_HANDLE_T update = vc_dispmanx_update_start(0);
  for (int i = 0; i < MAX_ELEMENTS; i++) {
  struct frame_element *fe = &frame_elements[i];
  int w = fe->w, h = fe->h;
  float aspect = fe->aspect;
  if (!fe->x && !fe->y && fe->zoom == 0x200) {
    fullscreen_element = fe;
  }
  if (!fe->h) continue; 
  if (fe->src_rect_dirty || fe->dst_rect_dirty || needs_reinit) {
    VC_RECT_T srcRect, dstRect;
    vc_dispmanx_rect_set(&srcRect, 0, 0, w << 16, h << 16);
    int target_w = (w * screenX * 3 / 4 / screenY) * (aspect * h / w);
    vc_dispmanx_rect_set(&dstRect, (screenX - apply_zoom(fe, target_w)) / 2 + fe->x,
                         (screenY - apply_zoom(fe, h)) / 2 + fe->y, apply_zoom(fe, target_w), apply_zoom(fe, h));
    if (fe->element) {
    vc_dispmanx_element_change_source(update, fe->element, fe->resource);
    vc_dispmanx_element_change_attributes(update, fe->element, ELEMENT_CHANGE_SRC_RECT | ELEMENT_CHANGE_DEST_RECT, 0, 0,
                                          &dstRect, &srcRect, 0, 0);
    } else {
      int layer = 10;
      fe->element = vc_dispmanx_element_add(update, display, layer, &dstRect, fe->resource, &srcRect, DISPMANX_PROTECTION_NONE,
                                    NULL, NULL, DISPMANX_NO_ROTATE);
    }
    rt_log("Resizing canvas %d %dx%d => %d(%d)x%d %f pixel aspect = %f frame time = %d (%.2ffps)\n", i, fe->w, fe->h, w, target_w, h,
           aspect, aspect * h / w, fe->time_us, 1000000./fe->time_us);
    fe->dst_rect_dirty = fe->src_rect_dirty = 0;
  }
  }
  vc_dispmanx_update_submit_sync(update);
  for (int i = 0; i < MAX_ELEMENTS; i++) {
  struct frame_element *fe = &frame_elements[i];
  if (fe->old_resource) {
        vc_dispmanx_resource_delete(fe->old_resource);
        fe->old_resource = 0;
  }
  }
  needs_reinit = 0;
  pthread_mutex_unlock(&update_mutex);
  if (fullscreen_element != prev_fullscreen_element || (fullscreen_element && fullscreen_element->framerate_dirty)) {
    if (fullscreen_element) {
      sdtv_set_filtering(0);
      int is_50hz = abs(fullscreen_element->time_us - 20000) < 200;
      int is_interlaced = fullscreen_element->h > (is_50hz ? 288 : 240);
      if (!is_50hz) {
        pal60_hack = 1;
      }
      sdtv_change_mode(is_50hz, is_interlaced);
      fullscreen_element->framerate_dirty = 0;
    } else {
      sdtv_set_filtering(1);
      sdtv_set_mode(original_mode);
    }
  }
  prev_fullscreen_element = fullscreen_element;
  struct timespec ts;
  clock_gettime(CLOCK_REALTIME, &ts);
  ts.tv_nsec += 100000000;
  if (ts.tv_nsec >= 1000000000) {
    ts.tv_sec += 1;
    ts.tv_nsec -= 1000000000;
  }
  pthread_mutex_lock(&vsync_mutex);
  pthread_cond_timedwait(&vsync_cond, &vsync_mutex, &ts);
  pthread_mutex_unlock(&vsync_mutex);
}
void dispmanx_update_frame(int idx, struct video_frame *frame) {
  struct frame_element *fe = &frame_elements[idx];
  int pitch_w = frame->pitch / pixel_format_to_size(frame->fmt);
  if (frame->pitch != fe->pitch || frame->w != fe->w || frame->h != fe->h || frame->fmt != fe->mode) {
    fe->src_rect_dirty = 1;
    if (fe->resource) {
      assert(fe->old_resource == 0);
      if (fe->old_resource) {
        vc_dispmanx_resource_delete(fe->resource);
      } else {
        fe->old_resource = fe->resource;
      }
    }
    uint32_t vc_image_ptr = 0;
    fe->resource =
        vc_dispmanx_resource_create(pixel_format_to_mode(frame->fmt), pitch_w, frame->h, &vc_image_ptr);
        fe->h = frame->h;
        fe->w = frame->w;
        fe->pitch = frame->pitch;
        fe->mode = frame->fmt;
  }
  float aspect = frame->aspect ? frame->aspect : frame->h ? (float)frame->w / frame->h : 1; 
  if (fe->aspect != aspect) {
    fe->dst_rect_dirty = 1;
    fe->aspect = aspect;
  }
  if (fe->time_us != frame->time_us) {
    fe->time_us = frame->time_us;
    fe->framerate_dirty = 1;
  }
  VC_RECT_T bmpRect;
  vc_dispmanx_rect_set(&bmpRect, 0, 0, pitch_w, frame->h);
  vc_dispmanx_resource_write_data(fe->resource, pixel_format_to_mode(fe->mode), frame->pitch, frame->data, &bmpRect);
}

int sdtv_aspect = 0;
void tvservice_callback(void *data, uint32_t reason, uint32_t p1, uint32_t p2) {
  if ((reason & VC_SDTV_NTSC) && pal60_hack) {
    engage_pal60();
    pal60_hack = 0;
  }
  if (reason & (VC_SDTV_NTSC | VC_SDTV_PAL)) {
    rt_log("video mode updated, needs dispmanx reinit\n");
    pthread_mutex_lock(&update_mutex);
    needs_reinit = 1;
    pthread_mutex_unlock(&update_mutex);
    pthread_cond_signal(&vsync_cond);
  }
}
static void dispmanx_vsync() {
  pthread_cond_signal(&vsync_cond);
}
void dispmanx_init() {
  bcm_host_init();
  original_mode = dispmanx_display_init();
  vc_tv_register_callback(tvservice_callback, 0);
  rt_log("DISPMANX INITED\n");
}
static int dispmanx_display_init() {
  u_int32_t displayNumber = 0;
  display = vc_dispmanx_display_open(displayNumber);
  assert(display != 0);
  TV_DISPLAY_STATE_T tvstate;
  vc_tv_get_display_state(&tvstate);

  DISPMANX_MODEINFO_T display_info;
  int ret = vc_dispmanx_display_get_info(display, &display_info);
  assert(ret == 0);
  screenX = display_info.width;
  screenY = display_info.height;
  int aspectX = 16;
  int aspectY = 9;
  if (tvstate.state & (VC_HDMI_HDMI | VC_HDMI_DVI))
    switch (tvstate.display.hdmi.aspect_ratio) {
    case HDMI_ASPECT_4_3:
      aspectX = 4;
      aspectY = 3;
      break;
    case HDMI_ASPECT_14_9:
      aspectX = 14;
      aspectY = 9;
      break;
    default:
    case HDMI_ASPECT_16_9:
      aspectX = 16;
      aspectY = 9;
      break;
    case HDMI_ASPECT_5_4:
      aspectX = 5;
      aspectY = 4;
      break;
    case HDMI_ASPECT_16_10:
      aspectX = 16;
      aspectY = 10;
      break;
    case HDMI_ASPECT_15_9:
      aspectX = 15;
      aspectY = 9;
      break;
    case HDMI_ASPECT_64_27:
      aspectX = 64;
      aspectY = 27;
      break;
    }
  else
    switch (tvstate.display.sdtv.display_options.aspect) {
    default:
    case SDTV_ASPECT_4_3:
      aspectX = 4, aspectY = 3;
      break;
    case SDTV_ASPECT_14_9:
      aspectX = 14, aspectY = 9;
      break;
    case SDTV_ASPECT_16_9:
      aspectX = 16, aspectY = 9;
      break;
    }
  sdtv_aspect = tvstate.display.sdtv.display_options.aspect;
  screenXoffset = (screenX - screenX * aspectY * 4 / 3 / aspectX) / 2;
  vc_dispmanx_vsync_callback(display, dispmanx_vsync, NULL);
  return tvstate.display.sdtv.mode;
}
void dispmanx_close() {
  int result;
  DISPMANX_UPDATE_HANDLE_T update = vc_dispmanx_update_start(0);
  for (int i = 0; i < MAX_ELEMENTS; i++) {
    if (frame_elements[i].element)
      result = vc_dispmanx_element_remove(update, frame_elements[i].element);
  }
  result = vc_dispmanx_update_submit_sync(update);
  for (int i = 0; i < MAX_ELEMENTS; i++) {
  if (frame_elements[i].resource) {
    result = vc_dispmanx_resource_delete(frame_elements[i].resource);
    assert(result == 0);
  }
  if (frame_elements[i].old_resource) {
    result = vc_dispmanx_resource_delete(frame_elements[i].old_resource);
    assert(result == 0);
  }
  }
  if (display) {
    result = vc_dispmanx_display_close(display);
    assert(result == 0);
  }
  vc_dispmanx_vsync_callback(display, NULL, NULL);
  sdtv_set_mode(original_mode);
}
