#ifndef MAIN_H
#define MAIN_H
#include <stdint.h>
#include "libretro.h"
enum rt_controls {
    JOYPAD_BUTTONS = 0,
    JOYPAD_LEFT_X = 1,
    JOYPAD_LEFT_Y = 2,
    JOYPAD_RIGHT_X = 3,
    JOYPAD_RIGHT_Y = 4,
    CONTROLS_MAX = 5,
    MOUSE_BUTTONS = 5, // reuse 0?
    MOUSE_X = 6, // reuse 1?
    MOUSE_Y = 7, // reuse 2?
    POINTER_X = 8, // lightpen/pointer
    POINTER_Y = 9, // lightpen/pointer
    LIGHT_BUTTONS = 10, // reuse 0?
    POINTER_PRESSED = 11, // reuse 0?
    POINTER_COUNT = 12, // what is this?
};


enum rt_message_type {
    LOAD_GAME = 1, // + path
    START_GAME,
    PAUSE_GAME,
    LOAD_STATE,
    SAVE_STATE, // + save state path
    UNLOAD_GAME,
    FF_GAME,
    INPUT_DATA, // + events uint32
    KEYBOARD_DATA, // 320/8 = 40b or events uint32[]
    VIDEO_OUT, // uint32[]
    AUDIO_OUT,
    OPTION_SET, // char*[]
    VSYNC,

    OPTION_DESC = 1,
};

struct message_keyboard_data {
        int type;
        uint8_t data[(RETROK_LAST + 7) >> 3];
    };
    struct message_input_data {
        int type;
        int port;
        int16_t data[CONTROLS_MAX];
    };
    struct message_video_out {
        int type;
        int posx, posy;
        int zoom;
    };

#endif
