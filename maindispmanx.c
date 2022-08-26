#include "libretro.h"
#include "mainlog.h"
#include <assert.h>
#include "maindispmanx.h"
#include <bcm_host.h>

#define ELEMENT_CHANGE_LAYER (1 << 0)
#define ELEMENT_CHANGE_OPACITY (1 << 1)
#define ELEMENT_CHANGE_DEST_RECT (1 << 2)
#define ELEMENT_CHANGE_SRC_RECT (1 << 3)
#define ELEMENT_CHANGE_MASK_RESOURCE (1 << 4)
#define ELEMENT_CHANGE_TRANSFORM (1 << 5)

static DISPMANX_DISPLAY_HANDLE_T display;
static DISPMANX_RESOURCE_HANDLE_T resource;
static DISPMANX_ELEMENT_HANDLE_T element;
#define SCREENX 384
#define SCREENY 288
static int screenX, screenY, screenXoffset;

int current_w, current_h, current_pitch;
int current_mode;
float current_aspect;
const void *current_screen;

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

int current_zoom = 0x200, current_dx, current_dy, needs_pos_update = 1;

void dispmanx_set_pos(int dx, int dy, int zoom) {
  needs_pos_update = current_zoom != zoom || current_dx != dx || current_dy != dy;
  current_zoom = zoom;
  current_dx = dx;
  current_dy = dy;
}
static int apply_zoom(int v) { return (v * current_zoom) >> 8; }

static void dispmanx_show(const char *buf, int w, int h, int pitch, float aspect, int new_mode) {
  if (buf)
    current_screen = buf;
  if (!aspect)
    aspect = (float)w / h;
  int pitch_w = pitch / pixel_format_to_size(new_mode);
  if (pitch != current_pitch || w != current_w || h != current_h || new_mode != current_mode || needs_pos_update) {
    uint32_t vc_image_ptr = 0;
    DISPMANX_RESOURCE_HANDLE_T new_resource =
        vc_dispmanx_resource_create(pixel_format_to_mode(new_mode), pitch_w, h, &vc_image_ptr);
    DISPMANX_UPDATE_HANDLE_T update = vc_dispmanx_update_start(0);
    VC_RECT_T srcRect, dstRect;
    vc_dispmanx_rect_set(&srcRect, 0, 0, w << 16, h << 16);
    int target_w = (w * screenX * 3 / 4 / screenY) * (aspect * h / w);
    vc_dispmanx_rect_set(&dstRect, (screenX - apply_zoom(target_w)) / 2 + current_dx,
                         (screenY - apply_zoom(h)) / 2 + current_dy, apply_zoom(target_w), apply_zoom(h));
    if (buf)
      vc_dispmanx_element_change_source(update, element, new_resource);
    vc_dispmanx_element_change_attributes(update, element, ELEMENT_CHANGE_SRC_RECT | ELEMENT_CHANGE_DEST_RECT, 0, 0,
                                          &dstRect, &srcRect, 0, 0);
    vc_dispmanx_update_submit_sync(update);
    if (resource)
      vc_dispmanx_resource_delete(resource);
    resource = new_resource;
    rt_log("Resizing canvas %dx%d => %d(%d)x%d %f pixel aspect = %f\n", current_w, current_h, w, target_w, h,
           aspect, aspect * h / w);
  }
  if (w != current_w || h != current_h || current_aspect != aspect) {
    // change dst rect
  }
  current_aspect = aspect;
  current_h = h;
  current_w = w;
  current_pitch = pitch;
  current_mode = new_mode;
  needs_pos_update = 0;
  //    DISPMANX_UPDATE_HANDLE_T update = vc_dispmanx_update_start(0);
  VC_RECT_T bmpRect;
  //	VC_RECT_T zeroRect;
  vc_dispmanx_rect_set(&bmpRect, 0, 0, pitch_w, h);
  if (buf)
    vc_dispmanx_resource_write_data(resource, pixel_format_to_mode(current_mode), pitch, (void *)buf, &bmpRect);
  //   vc_dispmanx_rect_set(&zeroRect, 0, 0, screenX, screenY);
  //   vc_dispmanx_element_change_attributes(update, element, ELEMENT_CHANGE_DEST_RECT, 0, 0, &zeroRect, 0, 0, 0);
  //	int result = vc_dispmanx_update_submit_sync(update); // This waits for vsync?
  //	assert(result == 0);
}
void dispmanx_show_frame(struct video_frame *frame) {
    dispmanx_show(frame->data, frame->w, frame->h, frame->pitch, frame->aspect, frame->fmt);
}

void dispmanx_show_last(void) {
    dispmanx_show(0, current_w, current_h, current_pitch, current_aspect, current_mode);
}


void dispmanx_init() {
  int32_t layer = 10;
  u_int32_t displayNumber = 0;
  int result = 0;

  // Init BCM
  bcm_host_init();

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
  screenXoffset = (screenX - screenX * aspectY * 4 / 3 / aspectX) / 2;

  // Create a resource and copy bitmap to resource
  uint32_t vc_image_ptr = 0;
  resource = vc_dispmanx_resource_create(VC_IMAGE_RGB565, 1, 1, &vc_image_ptr);

  assert(resource != 0);

  // Notify vc that an update is takng place
  DISPMANX_UPDATE_HANDLE_T update = vc_dispmanx_update_start(0);
  assert(update != 0);

  // Calculate source and destination rect values
  VC_RECT_T srcRect, dstRect;
  vc_dispmanx_rect_set(&srcRect, 0, 0, 1 << 16, 1 << 16);
  vc_dispmanx_rect_set(&dstRect, -1, -1, 1, 1);

  // Add element to vc
  element = vc_dispmanx_element_add(update, display, layer, &dstRect, resource, &srcRect, DISPMANX_PROTECTION_NONE,
                                    NULL, NULL, DISPMANX_NO_ROTATE);

  assert(element != 0);

  // Notify vc that update is complete
  result = vc_dispmanx_update_submit_sync(update); // This waits for vsync?
  assert(result == 0);
  rt_log("DISPMANX INITED\n");
}
void dispmanx_close() {
  int result;
  DISPMANX_UPDATE_HANDLE_T update = vc_dispmanx_update_start(0);
  if (element)
    result = vc_dispmanx_element_remove(update, element);
  result = vc_dispmanx_update_submit_sync(update);
  if (resource) {
    result = vc_dispmanx_resource_delete(resource);
    assert(result == 0);
  }
  if (display) {
    result = vc_dispmanx_display_close(display);
    assert(result == 0);
  }
}
