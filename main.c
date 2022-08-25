#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <dlfcn.h>
#include "libretro.h"
#include <stdarg.h>
#include <signal.h>
#include <bcm_host.h>
#include <assert.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <linux/input.h>
#include <sys/prctl.h>
#include "lz4.h"
#include "main.h"
#include "mainlog.h"

void retro_set_led_state(int led, int state) {
    rt_log("LED: %d %d\n", led, state);
}
void retro_log(enum retro_log_level level, const char *fmt, ...) {
    va_list args;
    va_start(args, fmt);
    rt_log_v(fmt, level, args);
    va_end(args);
}

#include <alsa/asoundlib.h>
snd_pcm_t *alsa;
 
void alsa_init(int fq) {
    int err;
    if ((err = snd_pcm_open(&alsa, "default", SND_PCM_STREAM_PLAYBACK, 0)) < 0) {
        printf("Playback open error: %s\n", snd_strerror(err));
        exit(EXIT_FAILURE);
    }
    if ((err = snd_pcm_set_params(alsa,
                      SND_PCM_FORMAT_S16,
                      SND_PCM_ACCESS_RW_INTERLEAVED,
                      2,
                      fq,
                      1,
                      200000)) < 0) {   /* 0.2sec */
        printf("Playback open error: %s\n", snd_strerror(err));
        exit(EXIT_FAILURE);
    } 
}
void alsa_write(const int16_t *buffer, int len) {
//    snd_pcm_prepare(alsa);
    snd_pcm_sframes_t f1 = -1,f2=-1;
    int e = snd_pcm_avail_delay(alsa, &f1, &f2);
    if (e == -32) {
        snd_pcm_prepare(alsa);
    }
    //rt_log("AUDIO BUFFER level %li, %li (%li) %d\n", f1, f2, f1+f2, e);
    snd_pcm_sframes_t frames = snd_pcm_writei(alsa, buffer, len);
        if (frames < 0) {
            printf("snd_pcm_writei failed: %s %d %li\n", snd_strerror(frames), len, frames);
            frames = snd_pcm_recover(alsa, frames, 0);
        } else if (frames > 0 && frames < len) {
            printf("Short write (expected %i, wrote %li)\n", len, frames);
        }
}


retro_keyboard_event_t retro_keyboard_event = 0;
uint8_t keyboardstate[RETROK_LAST];
#define MAX_PORTS 4
int16_t gamepad_state[MAX_PORTS][CONTROLS_MAX];



#define ELEMENT_CHANGE_LAYER (1<<0)
#define ELEMENT_CHANGE_OPACITY (1<<1)
#define ELEMENT_CHANGE_DEST_RECT (1<<2)
#define ELEMENT_CHANGE_SRC_RECT (1<<3)
#define ELEMENT_CHANGE_MASK_RESOURCE (1<<4)
#define ELEMENT_CHANGE_TRANSFORM (1<<5)

static DISPMANX_DISPLAY_HANDLE_T display;
static DISPMANX_RESOURCE_HANDLE_T resource;
static DISPMANX_ELEMENT_HANDLE_T element;
#define SCREENX 384
#define SCREENY 288
static int screenX, screenY, screenXoffset;

int current_w, current_h, current_pitch;
int current_mode;
double current_fps;
float current_aspect;
enum retro_pixel_format new_mode;
struct retro_system_av_info rsavi;
const void *current_screen;

VC_IMAGE_TYPE_T pixel_format_to_mode(enum retro_pixel_format fmt) {
    switch (fmt) {
        case RETRO_PIXEL_FORMAT_XRGB8888: return VC_IMAGE_XRGB8888;
        case RETRO_PIXEL_FORMAT_RGB565: return VC_IMAGE_RGB565;
        case RETRO_PIXEL_FORMAT_UNKNOWN:
        case RETRO_PIXEL_FORMAT_0RGB1555: break;
    }
    rt_log("pixel format not supported: %d\n", fmt);
    exit(0);
    return 0;
}
int pixel_format_to_size(enum retro_pixel_format fmt) {
    switch (fmt) {
        case RETRO_PIXEL_FORMAT_XRGB8888: return 4;
        case RETRO_PIXEL_FORMAT_RGB565: return 2;
        case RETRO_PIXEL_FORMAT_UNKNOWN:
        case RETRO_PIXEL_FORMAT_0RGB1555: break;
    }
    rt_log("pixel format not supported: %d\n", fmt);
    exit(0);
    return 0;
}

static int current_zoom = 0x200, current_dx, current_dy, needs_pos_update = 1;
static int apply_zoom(int v) {
    return (v * current_zoom) >> 8;
}


void dispmanx_show(const char *buf, int w, int h, int pitch) {
    if (buf) current_screen = buf;
    if (!rsavi.geometry.aspect_ratio) rsavi.geometry.aspect_ratio = (float)w/h;
    int pitch_w = pitch/pixel_format_to_size(new_mode);
    if (pitch != current_pitch || w != current_w || h != current_h || new_mode != current_mode || needs_pos_update) {
    	uint32_t vc_image_ptr = 0;
        DISPMANX_RESOURCE_HANDLE_T new_resource = vc_dispmanx_resource_create(pixel_format_to_mode(new_mode), pitch_w, h, &vc_image_ptr);
	    DISPMANX_UPDATE_HANDLE_T update = vc_dispmanx_update_start(0);
	    VC_RECT_T srcRect, dstRect;
    	vc_dispmanx_rect_set(&srcRect, 0, 0, w << 16, h << 16);
            int target_w = (w * screenX * 3 / 4 / screenY) * (rsavi.geometry.aspect_ratio*h/w);
    	    vc_dispmanx_rect_set(&dstRect, (screenX - apply_zoom(target_w))/2 + current_dx, (screenY-apply_zoom(h))/2 + current_dy, apply_zoom(target_w), apply_zoom(h));
        if (buf) vc_dispmanx_element_change_source(update, element, new_resource);
        vc_dispmanx_element_change_attributes(update, element, ELEMENT_CHANGE_SRC_RECT | ELEMENT_CHANGE_DEST_RECT, 0, 0, &dstRect, &srcRect, 0, 0);
	    vc_dispmanx_update_submit_sync(update);
        if (resource) vc_dispmanx_resource_delete(resource);
	    resource = new_resource;
        rt_log("Resizing canvas %dx%d => %d(%d)x%d %f pixel aspect = %f\n",current_w, current_h, w, target_w, h, rsavi.geometry.aspect_ratio, rsavi.geometry.aspect_ratio*h/w);
    }
    if (w != current_w || h != current_h || current_aspect != rsavi.geometry.aspect_ratio) {
        // change dst rect
    }
    current_aspect = rsavi.geometry.aspect_ratio;
    current_h = h;
    current_w = w;
    current_pitch = pitch;
    current_mode = new_mode;
    needs_pos_update = 0;
//    DISPMANX_UPDATE_HANDLE_T update = vc_dispmanx_update_start(0);
	VC_RECT_T bmpRect;
//	VC_RECT_T zeroRect;
    vc_dispmanx_rect_set(&bmpRect, 0, 0, pitch_w, h);
    if (buf) vc_dispmanx_resource_write_data(resource, pixel_format_to_mode(current_mode), pitch, (void*)buf, &bmpRect);
 //   vc_dispmanx_rect_set(&zeroRect, 0, 0, screenX, screenY);
 //   vc_dispmanx_element_change_attributes(update, element, ELEMENT_CHANGE_DEST_RECT, 0, 0, &zeroRect, 0, 0, 0);
//	int result = vc_dispmanx_update_submit_sync(update); // This waits for vsync?
//	assert(result == 0);
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
  if(tvstate.state & (VC_HDMI_HDMI | VC_HDMI_DVI)) switch (tvstate.display.hdmi.aspect_ratio) {
    case HDMI_ASPECT_4_3:   aspectX = 4;  aspectY = 3;  break;
    case HDMI_ASPECT_14_9:  aspectX = 14; aspectY = 9;  break;
    default:
    case HDMI_ASPECT_16_9:  aspectX = 16; aspectY = 9;  break;
    case HDMI_ASPECT_5_4:   aspectX = 5;  aspectY = 4;  break;
    case HDMI_ASPECT_16_10: aspectX = 16; aspectY = 10; break;
    case HDMI_ASPECT_15_9:  aspectX = 15; aspectY = 9;  break;
    case HDMI_ASPECT_64_27: aspectX = 64; aspectY = 27; break;
  } else switch (tvstate.display.sdtv.display_options.aspect) {
    default:
    case SDTV_ASPECT_4_3:  aspectX = 4, aspectY = 3;  break;
    case SDTV_ASPECT_14_9: aspectX = 14, aspectY = 9; break;
    case SDTV_ASPECT_16_9: aspectX = 16, aspectY = 9; break;
  }
  screenXoffset = (screenX - screenX * aspectY * 4 / 3 / aspectX) / 2;

	// Create a resource and copy bitmap to resource
	uint32_t vc_image_ptr = 0;
	resource = vc_dispmanx_resource_create(
		VC_IMAGE_RGB565, 1, 1, &vc_image_ptr);

	assert(resource != 0);


	// Notify vc that an update is takng place
	DISPMANX_UPDATE_HANDLE_T update = vc_dispmanx_update_start(0);
	assert(update != 0);

	// Calculate source and destination rect values
	VC_RECT_T srcRect, dstRect;
	vc_dispmanx_rect_set(&srcRect, 0, 0, 1 << 16, 1 << 16);
	vc_dispmanx_rect_set(&dstRect, -1, -1, 1, 1);

	// Add element to vc
	element = vc_dispmanx_element_add(
		update, display, layer, &dstRect, resource, &srcRect,
		DISPMANX_PROTECTION_NONE, NULL, NULL, DISPMANX_NO_ROTATE);


	assert(element != 0);
  

	// Notify vc that update is complete
	result = vc_dispmanx_update_submit_sync(update); // This waits for vsync?
	assert(result == 0);
    rt_log("DISPMANX INITED\n");
	//---------------------------------------------------------------------
}
void dispmanx_close() {
        int result;
	DISPMANX_UPDATE_HANDLE_T update = vc_dispmanx_update_start(0);
	if (element) result = vc_dispmanx_element_remove(update, element);
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

struct core_options {
    char *key;
    char *value;
} core_options[1024];
int num_core_options = 0;

const char *get_option(const char *key) {
    const struct core_options *option = core_options;
    for(; option; option++) {
//        rt_log("%p %s\n", option, option->key);
        if (!option->key) break;
        if (!strcmp(key, option->key)) {
//            rt_log("option found %s = %s\n", key, option->default_value);
            return option->value;
        }
    }
    rt_log("option not found %s\n", key);
//    exit(0);
    return NULL;
}
char framebuffer[1024*1024*100];
bool retro_environment(unsigned int cmd, void *data) {
//    rt_log("ENV: cmd %d\n", cmd & 255);
    switch(cmd) {
        case RETRO_ENVIRONMENT_GET_MESSAGE_INTERFACE_VERSION: {
            unsigned *version = data;
            *version = 0;
            rt_log("ENV: RETRO_ENVIRONMENT_GET_MESSAGE_INTERFACE_VERSION\n");
            return true;
        }
        case RETRO_ENVIRONMENT_GET_RUMBLE_INTERFACE: {
            //struct retro_rumble_interface *iface;
            rt_log("ENV: RETRO_ENVIRONMENT_GET_RUMBLE_INTERFACE\n");
            return false;            
        }
        case RETRO_ENVIRONMENT_GET_CAN_DUPE: {
            bool *can_dupe = data;
            *can_dupe = true;
            rt_log("ENV: RETRO_ENVIRONMENT_GET_CAN_DUPE\n");
            return true;
        }
        case RETRO_ENVIRONMENT_SET_PERFORMANCE_LEVEL: {
            const unsigned *level = data;
            rt_log("ENV: RETRO_ENVIRONMENT_SET_PERFORMANCE_LEVEL %d\n", *level);
            return true;
        }
        case RETRO_ENVIRONMENT_GET_LANGUAGE: {
            unsigned *lang = data;
            *lang = 0;
            rt_log("ENV: RETRO_ENVIRONMENT_GET_LANGUAGE\n");
            return true;
        }
        case RETRO_ENVIRONMENT_GET_SYSTEM_DIRECTORY: {
            void **ptr = data;
            rt_log("ENV: RETRO_ENVIRONMENT_GET_SYSTEM_DIRECTORY\n");
            *ptr = ".";
            return true;
        }
        case RETRO_ENVIRONMENT_GET_CORE_ASSETS_DIRECTORY: {
            void **ptr = data;
            rt_log("ENV: RETRO_ENVIRONMENT_GET_CORE_ASSETS_DIRECTORY\n");
            *ptr = ".";
            return true;
        }
        case RETRO_ENVIRONMENT_GET_SAVE_DIRECTORY: {
            void **ptr = data;
            rt_log("ENV: RETRO_ENVIRONMENT_GET_SAVE_DIRECTORY\n");
            *ptr = ".";
            return true;
        }
        case RETRO_ENVIRONMENT_GET_CORE_OPTIONS_VERSION: {
            unsigned *version = data;
            *version = 2;
            rt_log("ENV: RETRO_ENVIRONMENT_GET_CORE_OPTIONS_VERSION = %d\n", *version); 
            return true;
        }
        case RETRO_ENVIRONMENT_SET_VARIABLES: {
            const struct retro_variable *variables = data;
            rt_log("ENV: RETRO_ENVIRONMENT_SET_VARIABLES\n");
            for(; variables->key; variables++) {
                core_options[num_core_options].key = strdup(variables->key);
                const char *default_value = strstr(variables->value, "; ");
                if (!default_value) {
                    rt_log("BAD value %s\n", variables->value);
                    exit(0);
                }
                default_value+=2;
                const char *default_value_end = strchr(default_value, '|');
                if (!default_value_end) {
                    rt_log("BAD value %s\n", variables->value);
                    exit(0);
                }
                core_options[num_core_options].value = strndup(default_value, default_value_end - default_value);
                rt_log("ENV: VAR '%s' = (%s) '%s'\n", variables->key, core_options[num_core_options].value, variables->value);
                num_core_options++;
            }
            return true;
        }
        case RETRO_ENVIRONMENT_SET_CORE_OPTIONS: {
            const struct retro_core_option_definition *option = data;
            rt_log("ENV: RETRO_ENVIRONMENT_SET_CORE_OPTIONS\n");
            for(; option->key; option++) {
                  core_options[num_core_options].key = strdup(option->key);
                  core_options[num_core_options].value = strdup(option->default_value);
                  num_core_options++;
                rt_log("ENV: VAR '%s' = '%s'\n", option->key, option->default_value);
            }
            return true;
        }
        case RETRO_ENVIRONMENT_SET_CORE_OPTIONS_INTL: {
            rt_log("ENV: RETRO_ENVIRONMENT_SET_CORE_OPTIONS_INTL\n");
            const struct retro_core_option_definition **options_intl = data, *option = options_intl[0];
            for(; option->key; option++) {
                  core_options[num_core_options].key = strdup(option->key);
                  core_options[num_core_options].value = strdup(option->default_value);
                  num_core_options++;
                rt_log("ENV: VAR '%s' = '%s'\n", option->key, option->default_value);
            }
            return true;
        }
        case RETRO_ENVIRONMENT_SET_CORE_OPTIONS_V2: {
            const struct retro_core_options_v2 *option2 = data;
            const struct retro_core_option_v2_definition *option = option2->definitions;

            rt_log("ENV: RETRO_ENVIRONMENT_SET_CORE_OPTIONS_V2\n");
            for(; option->key; option++) {
                  core_options[num_core_options].key = strdup(option->key);
                  core_options[num_core_options].value = option->default_value ? strdup(option->default_value) : 0;
                  num_core_options++;
                rt_log("ENV: VAR%d '%s' = '%s'\n", num_core_options, option->key, option->default_value);
            }
            return true;
        }
        case RETRO_ENVIRONMENT_SET_CONTROLLER_INFO: {
            const struct retro_controller_info *info = data;
            rt_log("ENV: RETRO_ENVIRONMENT_SET_CONTROLLER_INFO\n");
            for (int i = 0; i < info->num_types; i++) {
                rt_log("ENV: %d: %s, %d\n", i, info->types[i].desc, info->types[i].id);
            }
            return true;
        }
        case RETRO_ENVIRONMENT_SET_SUPPORT_NO_GAME: {
            const bool * no_game = data;
            rt_log("ENV: RETRO_ENVIRONMENT_SET_SUPPORT_NO_GAME = %d\n", *no_game);
            return true;
        }
        case RETRO_ENVIRONMENT_GET_VFS_INTERFACE: {
            struct retro_vfs_interface_info *vfs_info = data;
            rt_log("ENV: RETRO_ENVIRONMENT_GET_VFS_INTERFACE v%d (not supported)\n", vfs_info->required_interface_version);
            return false;
        }
        case RETRO_ENVIRONMENT_GET_LED_INTERFACE: {
            struct retro_led_interface *led_info = data;
            led_info->set_led_state = retro_set_led_state;
            rt_log("ENV: RETRO_ENVIRONMENT_GET_LED_INTERFACE\n");
            return true;
        }
        case RETRO_ENVIRONMENT_GET_LOG_INTERFACE: {
            struct retro_log_callback *log_iface = data;
            log_iface->log = retro_log;
            rt_log("ENV: RETRO_ENVIRONMENT_GET_LOG_INTERFACE\n");
            return true;
        }
        case RETRO_ENVIRONMENT_GET_PERF_INTERFACE: {
            //struct retro_perf_callback *perf_iface = data;
            rt_log("ENV: RETRO_ENVIRONMENT_GET_PERF_INTERFACE (not supported)\n");
            return false;
        }
        case RETRO_ENVIRONMENT_GET_DISK_CONTROL_INTERFACE_VERSION: {
            unsigned *version = data;
            *version = 0;
            rt_log("ENV: RETRO_ENVIRONMENT_GET_DISK_CONTROL_INTERFACE_VERSION\n");
            return true;
        }
        case RETRO_ENVIRONMENT_SET_DISK_CONTROL_INTERFACE: {
            const struct retro_disk_control_callback *disk_ctrl = data;
            rt_log("ENV: RETRO_ENVIRONMENT_SET_DISK_CONTROL_INTERFACE %p\n", disk_ctrl->get_num_images);
            return true;
        }
        case RETRO_ENVIRONMENT_SET_SERIALIZATION_QUIRKS: {
            u_int64_t *q = data;
            rt_log("ENV: RETRO_ENVIRONMENT_SET_SERIALIZATION_QUIRKS %016lx\n", *q);
            return true;
        }
        case RETRO_ENVIRONMENT_SET_INPUT_DESCRIPTORS: {
//            const struct retro_input_descriptor *desc = data;
            rt_log("ENV: RETRO_ENVIRONMENT_SET_INPUT_DESCRIPTORS\n");
            // for (; desc->description; desc++) {
            //     rt_log("ENV:  %s\n", desc->description);
            // }
            return true;
        }
        case RETRO_ENVIRONMENT_SET_PIXEL_FORMAT: {
            const enum retro_pixel_format *fmt = data;
            new_mode = *fmt;
            rt_log("ENV: RETRO_ENVIRONMENT_SET_PIXEL_FORMAT %d\n", *fmt);
            return true;
        }
        case RETRO_ENVIRONMENT_GET_INPUT_BITMASKS: {
            //bool *b = data;
//            *b = true;
            rt_log("ENV: RETRO_ENVIRONMENT_GET_INPUT_BITMASKS %p\n", data);
            return true;
        }
        case RETRO_ENVIRONMENT_SET_FASTFORWARDING_OVERRIDE: {
            //const struct retro_fastforwarding_override *ff = data;
            rt_log("ENV: RETRO_ENVIRONMENT_SET_FASTFORWARDING_OVERRIDE\n");
            return true;
        }
        case RETRO_ENVIRONMENT_SET_KEYBOARD_CALLBACK: {
            const struct retro_keyboard_callback *cb = data;
            rt_log("ENV: RETRO_ENVIRONMENT_SET_KEYBOARD_CALLBACK %p\n", *cb);
            retro_keyboard_event = cb->callback;
            return true;
        }
        case RETRO_ENVIRONMENT_SET_SUPPORT_ACHIEVEMENTS: {
            const bool *value = data;
            rt_log("ENV: RETRO_ENVIRONMENT_SET_SUPPORT_ACHIEVEMENTS %d\n", *value);
            return false;
        }
        case RETRO_ENVIRONMENT_GET_VARIABLE: {
            struct retro_variable *var = data;
            if (!strcmp(var->key, "mame2003_skip_disclaimer")) {
                var->value = "enabled";
                return true;
            }
            // if (!strcmp(var->key, "atari800_system")) {
            //     var->value = "5200";
            //     return true;
            // }
            if (!strcmp(var->key, "vice_resid_sampling")) {
                var->value = "fast";
                return true;
            } else if (!strcmp(var->key, "cap32_scr_intensity")) {
                var->value = get_option(var->key);
                rt_log("ENV: RETRO_ENVIRONMENT_GET_VARIABLE %s = %s\n", var->key, var->value);
                return false;
            }
            var->value = get_option(var->key);
//            rt_log("ENV: RETRO_ENVIRONMENT_GET_VARIABLE %s = %s\n", var->key, var->value);
            return true;
        }
        case RETRO_ENVIRONMENT_SET_CORE_OPTIONS_DISPLAY: {
            //struct retro_core_option_display *opt = data;
//            rt_log("ENV: RETRO_ENVIRONMENT_SET_CORE_OPTIONS_DISPLAY %s %d\n", opt->key, opt->visible);
            return true;
        }
        case RETRO_ENVIRONMENT_GET_VARIABLE_UPDATE: {
            //bool *updated = data;
  //          rt_log("ENV: RETRO_ENVIRONMENT_GET_VARIABLE_UPDATE %d\n", *updated);
            return false;
        }
        case RETRO_ENVIRONMENT_SET_SYSTEM_AV_INFO: {
            const struct retro_system_av_info *avi = data;
            rsavi = *avi;
            rt_log("ENV: RETRO_ENVIRONMENT_SET_SYSTEM_AV_INFO %d x %d %lf\n", avi->geometry.base_height, avi->geometry.base_width, avi->timing.fps);
            return true;
        }
        case RETRO_ENVIRONMENT_SET_MEMORY_MAPS: {
            //const struct retro_memory_map * map = data;
            rt_log("ENV: RETRO_ENVIRONMENT_SET_MEMORY_MAPS\n");
            return false;
        }
        case RETRO_ENVIRONMENT_SET_MESSAGE: {
            const struct retro_message *msg = data;
            rt_log("ENV: RETRO_ENVIRONMENT_SET_MESSAGE '%s' for %d\n", msg->msg, msg->frames);
            return true;
        }
        case RETRO_ENVIRONMENT_SET_GEOMETRY: {
            const struct retro_game_geometry *info = data;
            rsavi.geometry = *info;
            rt_log("ENV: RETRO_ENVIRONMENT_SET_GEOMETRY %dx%d %.4f/9\n", info->base_width, info->base_height, info->aspect_ratio*9);
            return true;
        }
        case RETRO_ENVIRONMENT_GET_CURRENT_SOFTWARE_FRAMEBUFFER: {
            // struct retro_framebuffer *buf = data;
            // buf->data = framebuffer;
            // buf->memory_flags = 0;
            // buf->pitch = buf->width * 2;
            // buf->format = RETRO_PIXEL_FORMAT_RGB565;
//            rt_log("ENV: RETRO_ENVIRONMENT_GET_CURRENT_SOFTWARE_FRAMEBUFFER\n");
//            return true;
            return false;
        }
        case RETRO_ENVIRONMENT_GET_AUDIO_VIDEO_ENABLE: {
            //int *bits = data;
            // * Bit 0 (value 1): Enable Video
            // * Bit 1 (value 2): Enable Audio
            // * Bit 2 (value 4): Use Fast Savestates.
            // * Bit 3 (value 8): Hard Disable Audio
            return false;
        }
    }
    rt_log("ENV: unkown cmd %d\n", cmd & 255);
//    exit(0);
    return false;
} 
int dry_run;
void retro_video_refresh(const void *data, unsigned int w, unsigned int h, size_t pitch) {
    static int frame = 0;
    if (dry_run) return;
    if (frame < 10 || frame % 100 == 0) rt_log("VID%d: %d x %d pitch %d\n", frame, w, h, pitch);
    frame++;
    if (!data) {
        return;
    }
    dispmanx_show(data, w, h, pitch);
}
#define MAX_AUDIO_SAMPLES 1000
int16_t audiobuf[MAX_AUDIO_SAMPLES * 2];
int audiobufpos = 0;
void retro_audio_sample(int16_t l, int16_t r) {
      if (audiobufpos == MAX_AUDIO_SAMPLES) {
          alsa_write(audiobuf, audiobufpos);
          //printf("%d %d\n", (int)audiobuf[0], (int)audiobuf[1]);
          audiobufpos = 0;
      }
    audiobuf[audiobufpos * 2] = l;
    audiobuf[audiobufpos * 2 + 1] = r;
    audiobufpos++;
//    rt_log("AUD: 1 sample\n");
//    used by atari800
}
size_t retro_audio_sample_batch(const int16_t *data, size_t frames) {
//    rt_log("AUD: batch %d samples\n", frames);
    alsa_write(data, frames);
    //printf("%d %d\n", (int)data[0], (int)data[1]);
    return 0;
}
void retro_input_poll(void) {
//    rt_log("INP: poll\n");
}
int16_t retro_input_state(unsigned int port, unsigned int device, unsigned int index, unsigned int id) {
//    rt_log("INP: get state %d/%d/%d/%d\n", port, device, index, id);
    if (device == RETRO_DEVICE_KEYBOARD) {
        return (keyboardstate[id >> 3] >> (id & 7)) & 1;
    }
    if (port < MAX_PORTS) {
        int16_t *port_state = gamepad_state[port];
        if (id == RETRO_DEVICE_ID_JOYPAD_MASK) return port_state[JOYPAD_BUTTONS];
        if (id == RETRO_DEVICE_ID_ANALOG_X && index == RETRO_DEVICE_INDEX_ANALOG_LEFT) return port_state[JOYPAD_LEFT_X];
        if (id == RETRO_DEVICE_ID_ANALOG_Y && index == RETRO_DEVICE_INDEX_ANALOG_LEFT) return port_state[JOYPAD_LEFT_Y];
        if (id == RETRO_DEVICE_ID_ANALOG_X && index == RETRO_DEVICE_INDEX_ANALOG_RIGHT) return port_state[JOYPAD_RIGHT_X];
        if (id == RETRO_DEVICE_ID_ANALOG_Y && index == RETRO_DEVICE_INDEX_ANALOG_RIGHT) return port_state[JOYPAD_RIGHT_Y];
        return port_state[JOYPAD_BUTTONS] & (1 << id) ? 1 : 0;
    }
    return 0;
}
struct retro_core {
    void *handle;
    struct retro_system_info rsi;
    void (*retro_set_environment)(retro_environment_t);
    void (*retro_set_video_refresh)(retro_video_refresh_t);
    void (*retro_set_audio_sample)(retro_audio_sample_t);
    void (*retro_set_audio_sample_batch)(retro_audio_sample_batch_t);
    void (*retro_set_input_poll)(retro_input_poll_t);
    void (*retro_set_input_state)(retro_input_state_t);
    void (*retro_init)(void);
    void (*retro_deinit)(void);
    unsigned (*retro_api_version)(void);
    void (*retro_get_system_info)(struct retro_system_info *info);
    void (*retro_get_system_av_info)(struct retro_system_av_info *info);
    void (*retro_set_controller_port_device)(unsigned port, unsigned device);
    void (*retro_reset)(void);
    void (*retro_run)(void);
    size_t (*retro_serialize_size)(void);
    bool (*retro_serialize)(void *data, size_t size);
    bool (*retro_unserialize)(const void *data, size_t size);
    void (*retro_cheat_reset)(void);
    void (*retro_cheat_set)(unsigned index, bool enabled, const char *code);
    bool (*retro_load_game)(const struct retro_game_info *game);
    bool (*retro_load_game_special)(unsigned game_type, const struct retro_game_info *info, size_t num_info);
    void (*retro_unload_game)(void);
    unsigned (*retro_get_region)(void);
    void *(*retro_get_memory_data)(unsigned id);
    size_t (*retro_get_memory_size)(unsigned id);
} core;

int load_core(struct retro_core *core, const char *lib_file_path) {
//    core->handle = dlmopen(LM_ID_NEWLM, lib_file_path, RTLD_LAZY|RTLD_LOCAL);
    core->handle = dlopen(lib_file_path, RTLD_LAZY|RTLD_LOCAL);
    if (!core->handle) {
        rt_log("dlopen error: %s\n", dlerror());
        return -1;
    }
#define LOAD_SYM(name)    *(void**)(&core->name) = dlsym(core->handle, #name); if(!core->name) { rt_log("core symbol missing: %s\n", #name); dlclose(core->handle); return -1; }

    LOAD_SYM(retro_set_environment)
    LOAD_SYM(retro_set_video_refresh)
    LOAD_SYM(retro_set_audio_sample)
    LOAD_SYM(retro_set_audio_sample_batch)
    LOAD_SYM(retro_set_input_poll)
    LOAD_SYM(retro_set_input_state)
    LOAD_SYM(retro_init)
    LOAD_SYM(retro_deinit)
    LOAD_SYM(retro_api_version)
    LOAD_SYM(retro_get_system_info)
    LOAD_SYM(retro_get_system_av_info)
    LOAD_SYM(retro_set_controller_port_device)
    LOAD_SYM(retro_reset)
    LOAD_SYM(retro_run)
    LOAD_SYM(retro_serialize_size)
    LOAD_SYM(retro_serialize)
    LOAD_SYM(retro_unserialize)
    LOAD_SYM(retro_cheat_reset)
    LOAD_SYM(retro_cheat_set)
    LOAD_SYM(retro_load_game)
    LOAD_SYM(retro_load_game_special)
    LOAD_SYM(retro_unload_game)
    LOAD_SYM(retro_get_region)
    LOAD_SYM(retro_get_memory_data)
    LOAD_SYM(retro_get_memory_size)
    struct retro_system_info *rsi = &core->rsi;
    core->retro_get_system_info(rsi);
    rt_log("%s %s %s fullpath=%d\n", rsi->library_name, rsi->library_version, rsi->valid_extensions, rsi->need_fullpath);
    return 0;
}
        char state[1024*1024*20];
            char game_data[1024*1024*20];
struct game_state {
    struct state_header {
        int32_t w, h, pitch, fmt;
        float aspect;
        int32_t state_data_size;
        int32_t screen_data_size;
    } header;
    char screen[1024*1024*10];
    char state[1024*1024*20];
} game_state;
char state_tmp[1024*1024*10];
void save_state(const char *name) {
    game_state.header.aspect = current_aspect;
    game_state.header.w = current_w;
    game_state.header.h = current_h;
    game_state.header.pitch = current_pitch;
    game_state.header.fmt = current_mode;
    game_state.header.screen_data_size = current_h * current_pitch;
    game_state.header.state_data_size = core.retro_serialize_size();
    if (current_screen == game_state.screen) {
        memcpy(state_tmp, current_screen, game_state.header.screen_data_size);
        current_screen = state_tmp;
    }
    int csize = LZ4_compress_default(current_screen, game_state.screen, game_state.header.screen_data_size, sizeof(game_state.screen));
    rt_log("compressed state size %d => %d\n", game_state.header.screen_data_size, csize);
    game_state.header.screen_data_size = csize;
    core.retro_serialize(state_tmp, game_state.header.state_data_size);
    rt_log("state serialized (size = %d)\n", game_state.header.state_data_size);
    int csize2 = LZ4_compress_default(state_tmp, game_state.state, game_state.header.state_data_size, sizeof(game_state.state));
    rt_log("compressed state to %d\n", csize2);
    game_state.header.state_data_size = csize2;
    int fd = open(name, O_WRONLY | O_CREAT, 0666);
    if (fd < 0) return;
    write(fd, &game_state.header, sizeof(struct state_header));
    write(fd, game_state.screen, game_state.header.screen_data_size);
    write(fd, game_state.state, game_state.header.state_data_size);
    rt_log("state written to file\n");
}
void load_state_1(const char *name) {
    int fd = open(name, O_RDONLY);
    if (fd < 0) return;
    rt_log("state loading start 1\n");
    read(fd, &game_state.header, sizeof(struct state_header));
    if (1) {
        read(fd, state_tmp, game_state.header.screen_data_size);
        LZ4_decompress_safe(state_tmp, game_state.screen, game_state.header.screen_data_size, sizeof(game_state.screen));

        rsavi.geometry.aspect_ratio = game_state.header.aspect;
        new_mode = game_state.header.fmt;
        rt_log("state loading start 2\n");
        dispmanx_show(game_state.screen, game_state.header.w, game_state.header.h, game_state.header.pitch);
    } else {
        lseek(fd, game_state.header.screen_data_size, SEEK_CUR);
    }
    rt_log("state loading start 3\n");
    read(fd, state_tmp, game_state.header.state_data_size);
    int dsize2 = LZ4_decompress_safe(state_tmp, game_state.state, game_state.header.state_data_size, sizeof(game_state.state));
    game_state.header.state_data_size = dsize2;
    rt_log("state loaded (size = %d)\n", game_state.header.state_data_size);
}
void load_state_2() {
    if (!game_state.header.state_data_size) return;
    core.retro_unserialize(game_state.state, game_state.header.state_data_size);
    rt_log("state loaded 2 (size = %d)\n", game_state.header.state_data_size);
}
int load_game(const char *path) {
    struct retro_game_info game;
    bool result;
    if (path) {
        game.path = path;
        game.meta = 0;
        game.data = 0;
        game.size = 0;
        if (!core.rsi.need_fullpath) {
            FILE * f = fopen (path, "rb");
            game.size = fread (game_data, 1, sizeof(game_data), f);
            game.data = game_data;
            fclose(f);
            rt_log("game_loaded size = %d\n", game.size);
        }
        result = core.retro_load_game(&game);
    } else {
        result = core.retro_load_game(NULL);
    }
//    core.retro_set_controller_port_device(0, RETRO_DEVICE_SUBCLASS(RETRO_DEVICE_ANALOG, 0));
    core.retro_set_controller_port_device(0, RETRO_DEVICE_JOYPAD);
    core.retro_set_controller_port_device(1, RETRO_DEVICE_JOYPAD);
    if (!result) {
        rt_log("game load failed\n");
        return -1;
    }
    rt_log("game loaded\n");
    core.retro_get_system_av_info(&rsavi);
    rt_log("%d x %d, fps %lf, aspect %f audio fq %lf\n", rsavi.geometry.base_width, rsavi.geometry.base_height, rsavi.timing.fps, rsavi.geometry.aspect_ratio, rsavi.timing.sample_rate);
    alsa_init(rsavi.timing.sample_rate);
    return 0;
}
int comm_socket = -1;
int play_state = 0;
void setnonblocking(int sock) {
    int opt;

    opt = fcntl(sock, F_GETFL);
    if (opt < 0) {
        printf("fcntl(F_GETFL) fail.");
    }
    opt |= O_NONBLOCK;
    if (fcntl(sock, F_SETFL, opt) < 0) {
        printf("fcntl(F_SETFL) fail.");
    }
}
static int done = 0;
void read_comm() {
    if (comm_socket == -1) return;
    setnonblocking(comm_socket);
    char buf[4096];
    while(read(comm_socket, buf, sizeof(buf)) > 0) {
        int msg_type = *(int*)buf;
        if (msg_type == KEYBOARD_DATA) {
            struct message_keyboard_data *k = (void*)buf;
            int len = sizeof(k->data);
            for (int i = 0; i < len; i++) if (keyboardstate[i] != k->data[i]) {
                if (retro_keyboard_event) for (int j = 0; j < 8; j++) {
                    int id = i * 8 + j;
                    if ((keyboardstate[i] ^ k->data[i]) & (1<<j)) {
                        retro_keyboard_event(k->data[i] & (1<<j) ? 1 : 0, id, 0, 0);
                    }
                }
                keyboardstate[i] = k->data[i];
            }
        } else if (msg_type == INPUT_DATA) {
            struct message_input_data *k = (void*)buf;
//            rt_log("BTN %04X\n", k->data[JOYPAD_BUTTONS]);
            if (k->port < MAX_PORTS) memcpy(gamepad_state[k->port], k->data, sizeof(k->data));
        } else if (msg_type == VIDEO_OUT) {
            struct message_video_out *k = (void*)buf;
            needs_pos_update = current_zoom != k->zoom || current_dx != k->posx || current_dy != k->posy;
            current_zoom = k->zoom;
            current_dx = k->posx;
            current_dy = k->posy;
        } else if (msg_type == PAUSE_GAME) {
            play_state = 0;
        } else if (msg_type == START_GAME) {
            play_state = 1;
        } else if (msg_type == QUIT_CORE) {
            done = 1;
        } else {
            rt_log("GOT MESSAGE %d\n", msg_type);
        }
    };
}
#pragma GCC optimize ("O0")
int main(int argc, char** argv) {
    const char *cname = 0, *path = 0;
    const char *env_socket = getenv("S");
    rt_log_init('C');
    signal(SIGINT, SIG_IGN);
    if (env_socket) comm_socket = atoi(getenv("S"));
    char prname[16] = {0};
    snprintf(prname, 16, "rt: %s", argv[1]);
    prctl(PR_SET_NAME, (unsigned long)prname);
    rt_log("COMM SOCKET %d\n", comm_socket);
//    init_input();
//    rt_log("INPUT STARTED\n");
    dispmanx_init();

    if (argc < 2) {
        return 0;
    }
    if (!strcmp(argv[1], "cplus4")) {
//        cname = "/opt/retropie/libretrocores/lr-vice/vice_xplus4_libretro.so"; // ok, aspect bad? save state broken
        cname = "/home/pi/GIT/vice-libretro/vice_xplus4_libretro.so"; // ok, aspect bad?
        path = "/home/pi/treasure_island.prg";
    } else if (!strcmp(argv[1], "cplus4n")) {
//        cname = "/opt/retropie/libretrocores/lr-vice/vice_xplus4_libretro.so"; // ok, aspect bad? save state broken
        cname = "/home/pi/GIT/vice-libretro/vice_xplus4_libretro.so"; // ok, aspect bad?
    } else if(!strcmp(argv[1], "c64")) {
        cname = "./vice_x64_libretro.so"; // ok, aspect bad?
        path = "/home/pi/Simons_Basic.d64";
    } else if(!strcmp(argv[1], "c64x")) {
        cname = "/home/pi/GIT/vice-libretro/vice_x64_libretro.so"; // ok, aspect bad?
        path = "/home/pi/Simons_Basic.d64";
    } else if(!strcmp(argv[1], "c128")) {
        cname = "/opt/retropie/libretrocores/lr-vice/vice_x128_libretro.so"; // ok, aspect bad?
    } else if(!strcmp(argv[1], "amstrad")) {
        cname = "/opt/retropie/libretrocores/lr-caprice32/cap32_libretro.so"; // ok, aspect ok
    } else if(!strcmp(argv[1], "psx")) {
        cname = "/opt/retropie/libretrocores/lr-pcsx-rearmed/pcsx_rearmed_libretro.so"; // ok, aspect unknown
        path = "/home/pi/RetroPie/roms/psx/Castlevania.bin";
    } else if(!strcmp(argv[1], "nes")) {
        cname = "/opt/retropie/libretrocores/lr-fceumm/fceumm_libretro.so"; // ok, aspect unknown, configurable
        path = "./Metal Gear (USA).nes";
    } else if(!strcmp(argv[1], "amiga")) {
        cname = "/opt/retropie/libretrocores/lr-puae/puae_libretro.so"; // ok, aspect bad?  - cannot restore whdload save state, reconfigures terminal
//        path = "/home/pi/LupoAlberto_v1.01.lha";
        path = "/home/pi/Lupo Alberto (1991)(Idea).adf";
    } else if(!strcmp(argv[1], "gba")) {
        cname = "/opt/retropie/libretrocores/lr-mgba/mgba_libretro.so"; // ok, aspect ok
        path = "./Advance Wars 2.gba";
    } else if(!strcmp(argv[1], "gba2")) {
        cname = "/opt/retropie/libretrocores/lr-vba-next/vba_next_libretro.so"; // ok, aspect ok
        path = "./Advance Wars 2.gba";
    } else if(!strcmp(argv[1], "gb")) {
        cname = "/opt/retropie/libretrocores/lr-gambatte/gambatte_libretro.so"; // ok, aspect ok
        path = "./Dr. Mario (W) (V1.1).gb";
    } else if(!strcmp(argv[1], "zx")) {
        cname = "/opt/retropie/libretrocores/lr-fuse/fuse_libretro.so"; // ok, aspect 0, save state broken?
        path = "/home/pi//RetroPie/roms/zxspectrum/3D Deathchase (1983)(Zeppelin Games Ltd).tzx";
    } else if(!strcmp(argv[1], "atari")) {
        cname = "/opt/retropie/libretrocores/lr-atari800/atari800_libretro.so"; // ok, aspect unknown
        path = "/home/pi/Mr. Robot and His Robot Factory (1983)(Datamost)(US)[h SOL].atr";
    } else if(!strcmp(argv[1], "atari5200")) {
        cname = "/home/pi/GIT/libretro-atari800lib/libatari800-5200_libretro.so";
        path = "./mrrobot.atr";
    } else if(!strcmp(argv[1], "atari800")) {
        cname = "/home/pi/GIT/libretro-atari800lib/libatari800_libretro.so";
        path = "./mrrobot.atr";
//        path = "./Arkanoid.atr";
    } else if(!strcmp(argv[1], "atari2600")) {
        cname = "/opt/retropie/libretrocores/lr-stella2014/stella2014_libretro.so"; // ok, aspect unknown
        path = "/home/pi/RetroPie/roms/atari2600/Rive Raid.bin";
    } else if(!strcmp(argv[1], "snes")) {
       cname = "/opt/retropie/libretrocores/lr-snes9x2005/snes9x2005_libretro.so"; // ok
//        cname = "/opt/retropie/libretrocores/lr-snes9x/snes9x_libretro.so"; // gfx shifted
        path = "./Super Mario World (U) [!].smc";
    } else if(!strcmp(argv[1], "sega")) {
        cname = "/opt/retropie/libretrocores/lr-genesis-plus-gx/genesis_plus_gx_libretro.so";
        path = "./Aladdin (USA).md";
    } else if (!strcmp(argv[1], "scumm")) {
        cname = "/opt/retropie/libretrocores/lr-scummvm/scummvm_libretro.so"; // no saves
        path = "/home/pi/ij/ATLANTIS.000";
    } else if (!strcmp(argv[1], "mame")) {
        cname = "/opt/retropie/libretrocores/lr-mame2003/mame2003_libretro.so";
        path = "/home/pi/RetroPie/roms/mame/moonwlkb.zip";
    }
    if (argc >= 3) {
        path = argv[2];
    } 
    if (!cname) exit(0);
    // more: n64? lynx atarist g&w wanderswan (color) pokemonmini dosbox
    // save states: mame (run for one frame, 3 for audio), zx (run for one frame), atari800 (no saves supported), plus4 audio
    read_comm();
    rt_log("STARTED\n");
    char savename2[1000];
    if (path) {
        sprintf(savename2, "%s.save.rtube", path);
    } else {
        sprintf(savename2, "./nocontent-%s.save.rtube", argv[1]);
    }
    load_state_1(savename2);
    rt_log("save preloaded\n");

    int res = load_core(&core, cname);
    rt_log("CORE LOADED %d\n", res);

    core.retro_set_environment(retro_environment);
    core.retro_set_audio_sample(retro_audio_sample);
    core.retro_set_audio_sample_batch(retro_audio_sample_batch);
    core.retro_set_video_refresh(retro_video_refresh);
    core.retro_set_input_poll(retro_input_poll);
    core.retro_set_input_state(retro_input_state);
    rt_log("callbacks initialized\n");
    core.retro_init();
    rt_log("core initialized\n");

    if (load_game(path) < 0) return -1;

    if (game_state.header.state_data_size) {
        dry_run = 1;
        core.retro_run();
        core.retro_run();
        core.retro_run();
        core.retro_run();
        dry_run = 0;
    }
    load_state_2();

    int frames = 0;
    for(int i = 0; !done; i++) {
        read_comm();
        int64_t s = timestamp();
        if (play_state) {
            core.retro_run();
            frames++;
        }
        else dispmanx_show(0, current_w, current_h, current_pitch);
        int64_t frame_time_ms = 1000000 / rsavi.timing.fps;
        int64_t d = timestamp() - s;
        if (frame_time_ms > d) usleep(frame_time_ms - d);
    }
    rt_log("core run\n");

    if (frames) save_state(savename2);

    core.retro_unload_game();
    rt_log("game unloaded\n");
    core.retro_deinit();
    rt_log("core deinitialized\n");
    dlclose(core.handle);
    dispmanx_close();

    return EXIT_SUCCESS;
}
#pragma GCC reset_options