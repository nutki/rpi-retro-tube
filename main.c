#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <dlfcn.h>
#include "libretro.h"
#include <stdarg.h>
#include <signal.h>
#include <assert.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <linux/input.h>
#include <sys/prctl.h>
#include "lz4.h"
#include "main.h"
#include "mainlog.h"
#include "maindispmanx.h"
#include <stdatomic.h>

#define acquire(m) while (atomic_flag_test_and_set(m))
#define release(m) atomic_flag_clear(m)

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
int audio_buffer_level = 0;
int audio_init = 0;
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
                      50000)) < 0) {   /* 0.05sec */
        printf("Playback open error: %s\n", snd_strerror(err));
        exit(EXIT_FAILURE);
    }
    audio_init = 1;
}
uint32_t tmp_audio_buffer[4096];
void alsa_write(const int16_t *buffer, int len) {
//    snd_pcm_prepare(alsa);
    snd_pcm_sframes_t f1 = -1,f2=-1;
    if (!alsa) {
        rt_log("audio frame without alsa (%d)\n", len);
        return;
    }
    int e = snd_pcm_avail_delay(alsa, &f1, &f2);
    if (e == -32) {
        snd_pcm_prepare(alsa);
        e = snd_pcm_avail_delay(alsa, &f1, &f2);
        rt_log("audio reset\n");
        audio_init = 1;
    }
    if (!e && f1 + f2) {
        audio_buffer_level = 100 * f2 / (f1 + f2);
    }
//    rt_log("AUDIO BUFFER level %li, %li (%li) %d %d +%d\n", f1, f2, f1+f2, e, audio_buffer_level, len);
    if (audio_init) {
//        snd_pcm_writei(alsa, tmp_audio_buffer, f1 - len);
        audio_init = 0;
    }
    snd_pcm_sframes_t frames = snd_pcm_writei(alsa, buffer, len);
        if (frames < 0) {
            printf("snd_pcm_writei failed: %s %d %li\n", snd_strerror(frames), len, frames);
            frames = snd_pcm_recover(alsa, frames, 0);
        } else if (frames > 0 && frames < len) {
            printf("Short write (expected %i, wrote %li)\n", len, frames);
        }
}

enum retro_pixel_format pixel_format;
struct retro_system_av_info rsavi;
retro_keyboard_event_t retro_keyboard_event = 0;
uint8_t keyboardstate[RETROK_LAST];

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
            pixel_format = *fmt;
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
            if (!strcmp(var->key, "duckstation_GPU.Renderer")) {
                var->value = "Software";
                return true;
            }
            if (!strcmp(var->key, "duckstation_Display.CropMode")) {
                var->value = "None";
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
struct video_frame *last_frame;
int frame_cnt = 0;
struct shared_memory *shared_mem;
int frame_sync;
float par50, par60;
int skip_frames;
struct par_table {
    int w, is50hz;
    float par;
} *par_table;
void retro_video_refresh(const void *data, unsigned int w, unsigned int h, size_t pitch) {
    static int frame = 0;
    if (dry_run) return;
    if (skip_frames) {
        skip_frames--;
        return;
    }
    int is50hz = rsavi.timing.fps > 48 && rsavi.timing.fps < 52;
    float par_override = is50hz ? par50 : par60;
    for (struct par_table *pt = par_table; pt && pt->w; pt++) {
        if (pt->w == w && pt->is50hz == is50hz) {
            par_override = pt->par;
            break;
        }
    }
    if (frame < 10 || frame % 100 == 0) rt_log("VID%d: %d x %d pitch %d par %.3f -> %.3f\n", frame, w, h, pitch, rsavi.geometry.aspect_ratio * h / w, par_override);
    frame++;
    if (!data) {
        return;
    }
    acquire(&last_frame->mutex);
    int t = 0;
    if (frame_sync) for (t = 0; last_frame->id && t < 25; t++) {
        release(&last_frame->mutex);
        usleep(1000);
        acquire(&last_frame->mutex);
    }
    if (frame_sync && last_frame->id) {
        rt_log("frame dropped\n");
    } else if (t) {
//        rt_log("frame would be dropped, waited %dms\n", t);
    }
    last_frame->w = w;
    last_frame->h = h;
    last_frame->pitch = pitch;
    last_frame->aspect = par_override ? par_override * w / h : rsavi.geometry.aspect_ratio;
    last_frame->fmt = pixel_format;
    last_frame->time_us = (int)(1000000/(rsavi.timing.fps ? rsavi.timing.fps : 1));
    last_frame->id = ++frame_cnt;
    memcpy(shared_mem->frame_data, data, h * pitch);
    release(&last_frame->mutex);
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
        return (shared_mem->keyboard_state[id >> 3] >> (id & 7)) & 1;
    }
    if (port >= MAX_PORTS) return 0;
    int16_t *port_state = shared_mem->joypad_state[port];
    if (device == RETRO_DEVICE_ANALOG) {
        if (id == RETRO_DEVICE_ID_ANALOG_X && index == RETRO_DEVICE_INDEX_ANALOG_LEFT) return port_state[JOYPAD_LEFT_X];
        if (id == RETRO_DEVICE_ID_ANALOG_Y && index == RETRO_DEVICE_INDEX_ANALOG_LEFT) return port_state[JOYPAD_LEFT_Y];
        if (id == RETRO_DEVICE_ID_ANALOG_X && index == RETRO_DEVICE_INDEX_ANALOG_RIGHT) return port_state[JOYPAD_RIGHT_X];
        if (id == RETRO_DEVICE_ID_ANALOG_Y && index == RETRO_DEVICE_INDEX_ANALOG_RIGHT) return port_state[JOYPAD_RIGHT_Y];
    }
    if (device == RETRO_DEVICE_JOYPAD || (device == RETRO_DEVICE_ANALOG && index == RETRO_DEVICE_INDEX_ANALOG_BUTTON)) {
        if (id == RETRO_DEVICE_ID_JOYPAD_MASK) return port_state[JOYPAD_BUTTONS];
        return port_state[JOYPAD_BUTTONS] & (1 << id) ? 1 : 0;
    }
    if (device == RETRO_DEVICE_MOUSE) {
//        rt_log("query mouse port %d = %d %d %d\n", port, port_state[MOUSE_BUTTONS], port_state[MOUSE_X], port_state[MOUSE_Y]);
        if (id == RETRO_DEVICE_ID_MOUSE_X) return port_state[MOUSE_X];
        if (id == RETRO_DEVICE_ID_MOUSE_Y) return port_state[MOUSE_Y];
        if (id <= RETRO_DEVICE_ID_MOUSE_BUTTON_5) return port_state[MOUSE_BUTTONS] & (1 << id) ? 1 : 0;
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
    game_state.header.aspect = last_frame->aspect;
    game_state.header.w = last_frame->w;
    game_state.header.h = last_frame->h;
    game_state.header.pitch = last_frame->pitch;
    game_state.header.fmt = last_frame->fmt;
    game_state.header.screen_data_size = last_frame->h * last_frame->pitch;
    game_state.header.state_data_size = core.retro_serialize_size();
    int csize = LZ4_compress_default((void *)shared_mem->frame_data, game_state.screen, game_state.header.screen_data_size, sizeof(game_state.screen));
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

        rt_log("state loading start 2\n");
        acquire(&last_frame->mutex);
        last_frame->w = game_state.header.w;
        last_frame->h = game_state.header.h;
        last_frame->pitch = game_state.header.pitch;
        last_frame->aspect = game_state.header.aspect;
        last_frame->fmt = game_state.header.fmt;
        last_frame->time_us = 1000000;
        last_frame->id = ++frame_cnt;
        memcpy(shared_mem->frame_data, game_state.screen, last_frame->h * last_frame->pitch);
        release(&last_frame->mutex);
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
void emit_key_events() {
    if (!retro_keyboard_event) return;
    int len = sizeof(shared_mem->keyboard_state);
    for (int i = 0; i < len; i++) if (keyboardstate[i] != shared_mem->keyboard_state[i]) {
        for (int j = 0; j < 8; j++) {
            int id = i * 8 + j;
            if ((keyboardstate[i] ^ shared_mem->keyboard_state[i]) & (1<<j)) {
                retro_keyboard_event(shared_mem->keyboard_state[i] & (1<<j) ? 1 : 0, id, 0, 0);
            }
        }
        keyboardstate[i] = shared_mem->keyboard_state[i];
    }
}
void read_comm() {
    if (comm_socket == -1) return;
    setnonblocking(comm_socket);
    char buf[4096];
    while(read(comm_socket, buf, sizeof(buf)) > 0) {
        int msg_type = *(int*)buf;
        if (msg_type == PAUSE_GAME) {
            play_state = 0;
        } else if (msg_type == START_GAME) {
            play_state = 1;
        } else if (msg_type == QUIT_CORE) {
            done = 1;
        } else if (msg_type == SYNC_ON) {
            frame_sync = 1;
            rt_log("frame sync ON\n");
        } else if (msg_type == SYNC_OFF) {
            frame_sync = 0;
            rt_log("frame sync OFF\n");
        } else {
            rt_log("GOT MESSAGE %d\n", msg_type);
        }
    };
}

/*
 1:1 NTSC P -> 6.136Mhz
 1:1 PAL P -> 7.375Mhz

 par = (1:1 dot clock) / (dot clock)

  PSX.256-pix Dotclock =  5.322240MHz (44100Hz*300h*11/7/10)
  PSX.320-pix Dotclock =  6.652800MHz (44100Hz*300h*11/7/8)
  PSX.368-pix Dotclock =  7.603200MHz (44100Hz*300h*11/7/7)
  PSX.512-pix Dotclock = 10.644480MHz (44100Hz*300h*11/7/5)
  PSX.640-pix Dotclock = 13.305600MHz (44100Hz*300h*11/7/4)
*/
// https://pineight.com/mw/page/Dot_clock_rates.xhtml
// https://pineight.com/mw/page/Talk_Dot_clock_rates.xhtml
// https://forums.atariage.com/topic/189995-confusion-with-aspect-ratios/
// https://www.nesdev.org/wiki/Overscan
// https://www.nesdev.org/wiki/Cycle_reference_chart
// https://wiki.superfamicom.org/timing
// https://codebase64.org/doku.php?id=base:pixel_aspect_ratio
// http://www.retroisle.com/general/tedchip.php
// https://retrocomputing.stackexchange.com/questions/25107/what-is-the-size-of-the-border-of-the-zx-spectrum-in-scanlines-pixels-bytes
struct par_table par_table_psx[] = {
    {256, 0, 1.143},
    {320, 0, 0.914},
    {368, 0, 0.800},
    {384, 0, 0.800},
    {512, 0, 0.571},
    {640, 0, 0.457},
    {256, 1, 1.386},
    {320, 1, 1.109},
    {368, 1, 0.970},
    {512, 1, 0.693},
    {640, 1, 0.554},
    {0},
};

struct core_info {
    char *name;
    char *path;
    int dry_run_frames, skip_frames;
    float par50, par60;
    struct par_table *par;
} cores[] = {
    { "cplus4", "/home/pi/GIT/vice-libretro/vice_xplus4_libretro.so" },
    { "c64", "./vice_x64_libretro.so" },
    { "c64x", "/home/pi/GIT/vice-libretro/vice_x64_libretro.so" },
    { "c128", "/opt/retropie/libretrocores/lr-vice/vice_x128_libretro.so" },
    { "amstrad", "/opt/retropie/libretrocores/lr-caprice32/cap32_libretro.so", skip_frames : 1 },
    { "psx", "/opt/retropie/libretrocores/lr-pcsx-rearmed/pcsx_rearmed_libretro.so", par : par_table_psx, skip_frames : 1 },
    { "psx-ds", "./duckstation_libretro.so" },
    { "nes", "/opt/retropie/libretrocores/lr-fceumm/fceumm_libretro.so", par60 : 1.1428, par50 : 1.3861 },
    { "amiga2", "./puae2021_libretro.so" },
    { "amiga", "/opt/retropie/libretrocores/lr-puae/puae_libretro.so" },
    { "gba", "/opt/retropie/libretrocores/lr-mgba/mgba_libretro.so" },
    { "gba2", "/opt/retropie/libretrocores/lr-vba-next/vba_next_libretro.so" },
    { "gb", "/opt/retropie/libretrocores/lr-gambatte/gambatte_libretro.so" },
    { "zx", "/opt/retropie/libretrocores/lr-fuse/fuse_libretro.so", dry_run_frames : 1 },
    { "atari", "/opt/retropie/libretrocores/lr-atari800/atari800_libretro.so" },
    { "atari5200", "/home/pi/GIT/libretro-atari800lib/libatari800-5200_libretro.so" },
    { "atari800", "/home/pi/GIT/libretro-atari800lib/libatari800_libretro.so" }, // par OK
    { "atari2600", "/opt/retropie/libretrocores/lr-stella2014/stella2014_libretro.so", par60 : 1.714 },
    { "snes", "/opt/retropie/libretrocores/lr-snes9x2005/snes9x2005_libretro.so", par60 : 1.1428, par50 : 1.3862 },
//  { "snes", "/opt/retropie/libretrocores/lr-snes9x/snes9x_libretro.so" }, // gfx shifted (pitch not multiple of 32)
    { "sega", "/opt/retropie/libretrocores/lr-genesis-plus-gx/genesis_plus_gx_libretro.so" }, // par60 OK
    { "mame", "/opt/retropie/libretrocores/lr-mame2003/mame2003_libretro.so", dry_run_frames : 3 },
    { "mame2000", "/opt/retropie/libretrocores/lr-mame2000/mame2000_libretro.so", dry_run_frames : 3 },
    { "mama2003plus", "/opt/retropie/libretrocores/lr-mame2003-plus/mame2003_plus_libretro.so", dry_run_frames : 3 },
    { "mame2010", "/opt/retropie/libretrocores/lr-mame2010/mame2010_libretro.so", dry_run_frames : 3 },
    // more: lynx atarist g&w wanderswan (color) pokemonmini dosbox
    { 0 },
};

#include <sys/mman.h>
#pragma GCC optimize ("O0")
int main(int argc, char** argv) {
    const char *cname = 0, *path = 0;
    const char *env_socket = getenv("S");
    const char *env_mmapfd = getenv("M");
    rt_log_init('C');
    signal(SIGINT, SIG_IGN);
    if (env_socket) comm_socket = atoi(getenv("S"));
    if (env_mmapfd) {
        int mem_fd = atoi(env_mmapfd);
        shared_mem = mmap(0, SHARED_MEM_SIZE, PROT_READ | PROT_WRITE, MAP_SHARED, mem_fd, 0);
        if (!shared_mem) return 0;
        last_frame = &shared_mem->frame;
        release(&last_frame->mutex);
    }
    char prname[16] = {0};
    snprintf(prname, 16, "rt: %s", argv[1]);
    prctl(PR_SET_NAME, (unsigned long)prname);
    rt_log("COMM SOCKET %d\n", comm_socket);
//    init_input();
//    rt_log("INPUT STARTED\n");

    if (argc < 2) {
        return 0;
    }
    int dry_run_frames = 0;
    for (struct core_info *c = cores; c->name; c++) if (!strcmp(c->name, argv[1])) {
        dry_run_frames = c->dry_run_frames;
        skip_frames = c->skip_frames;
        par50 = c->par50;
        par60 = c->par60;
        par_table = c->par;
        cname = c->path;
    }
    if (argc >= 3) {
        path = argv[2];
    } 
    if (!cname) exit(0);
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

    if (game_state.header.state_data_size && dry_run_frames) {
        dry_run = 1;
        for (int i = 0; i < dry_run_frames; i++) {
            core.retro_run();
        }
        dry_run = 0;
    }
    load_state_2();

    int frames = 0;
    for(int i = 0; !done; i++) {
        read_comm();
        int64_t s = timestamp();
        if (play_state) {
            emit_key_events();
            core.retro_run();
            frames++;
        }
        int64_t frame_time_ms = 1000000 / rsavi.timing.fps;
        int64_t d = timestamp() - s;
        if (frame_time_ms > d && !frame_sync) usleep(frame_time_ms - d);
    }
    rt_log("core run\n");

    if (frames) save_state(savename2);

    core.retro_unload_game();
    rt_log("game unloaded\n");
    core.retro_deinit();
    rt_log("core deinitialized\n");
    dlclose(core.handle);

    return EXIT_SUCCESS;
}
#pragma GCC reset_options