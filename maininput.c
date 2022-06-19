#include <unistd.h>
#include <fcntl.h>
#include <linux/input.h>

#include "libretro.h"
#include "maininput.h"

int keymap[KEY_MAX] = {
    [KEY_BACKSPACE]=RETROK_BACKSPACE,
    [KEY_TAB]=RETROK_TAB,
    [KEY_CLEAR]=RETROK_CLEAR,
    //[KEY_UNKNOWN]=RETROK_RETURN,
    [KEY_PAUSE]=RETROK_PAUSE,
    [KEY_ESC]=RETROK_ESCAPE,
    [KEY_SPACE]=RETROK_SPACE,
    //[KEY_UNKNOWN]=RETROK_EXCLAIM,
    //[KEY_UNKNOWN]=RETROK_QUOTEDBL,
    //[KEY_UNKNOWN]=RETROK_HASH,
    [KEY_DOLLAR]=RETROK_DOLLAR,
    //[KEY_UNKNOWN]=RETROK_AMPERSAND,
    //[KEY_UNKNOWN]=RETROK_QUOTE,
    //[KEY_UNKNOWN]=RETROK_LEFTPAREN,
    //[KEY_UNKNOWN]=RETROK_RIGHTPAREN,
    //[KEY_UNKNOWN]=RETROK_ASTERISK,
    //[KEY_UNKNOWN]=RETROK_PLUS,
    [KEY_COMMA]=RETROK_COMMA,
    [KEY_MINUS]=RETROK_MINUS,
    [KEY_DOT]=RETROK_PERIOD,
    [KEY_SLASH]=RETROK_SLASH,
    [KEY_0]=RETROK_0,
    [KEY_1]=RETROK_1,
    [KEY_2]=RETROK_2,
    [KEY_3]=RETROK_3,
    [KEY_4]=RETROK_4,
    [KEY_5]=RETROK_5,
    [KEY_6]=RETROK_6,
    [KEY_7]=RETROK_7,
    [KEY_8]=RETROK_8,
    [KEY_9]=RETROK_9,
    //[KEY_UNKNOWN]=RETROK_COLON,
    [KEY_SEMICOLON]=RETROK_SEMICOLON,
    //[KEY_UNKNOWN]=RETROK_LESS,
    [KEY_EQUAL]=RETROK_EQUALS,
    //[KEY_UNKNOWN]=RETROK_GREATER,
    [KEY_QUESTION]=RETROK_QUESTION,
    //[KEY_UNKNOWN]=RETROK_AT,
    [KEY_LEFTBRACE]=RETROK_LEFTBRACKET,
    [KEY_BACKSLASH]=RETROK_BACKSLASH,
    [KEY_RIGHTBRACE]=RETROK_RIGHTBRACKET,
    //[KEY_UNKNOWN]=RETROK_CARET,
    //[KEY_UNKNOWN]=RETROK_UNDERSCORE,
    [KEY_GRAVE]=RETROK_BACKQUOTE,
    [KEY_A]=RETROK_a,
    [KEY_B]=RETROK_b,
    [KEY_C]=RETROK_c,
    [KEY_D]=RETROK_d,
    [KEY_E]=RETROK_e,
    [KEY_F]=RETROK_f,
    [KEY_G]=RETROK_g,
    [KEY_H]=RETROK_h,
    [KEY_I]=RETROK_i,
    [KEY_J]=RETROK_j,
    [KEY_K]=RETROK_k,
    [KEY_L]=RETROK_l,
    [KEY_M]=RETROK_m,
    [KEY_N]=RETROK_n,
    [KEY_O]=RETROK_o,
    [KEY_P]=RETROK_p,
    [KEY_Q]=RETROK_q,
    [KEY_R]=RETROK_r,
    [KEY_S]=RETROK_s,
    [KEY_T]=RETROK_t,
    [KEY_U]=RETROK_u,
    [KEY_V]=RETROK_v,
    [KEY_W]=RETROK_w,
    [KEY_X]=RETROK_x,
    [KEY_Y]=RETROK_y,
    [KEY_Z]=RETROK_z,
    //[KEY_UNKNOWN]=RETROK_LEFTBRACE,
    //[KEY_UNKNOWN]=RETROK_BAR,
    //[KEY_UNKNOWN]=RETROK_RIGHTBRACE,
    //[KEY_UNKNOWN]=RETROK_TILDE,
    [KEY_DELETE]=RETROK_DELETE,

    [KEY_KP0]=RETROK_KP0,
    [KEY_KP1]=RETROK_KP1,
    [KEY_KP2]=RETROK_KP2,
    [KEY_KP3]=RETROK_KP3,
    [KEY_KP4]=RETROK_KP4,
    [KEY_KP5]=RETROK_KP5,
    [KEY_KP6]=RETROK_KP6,
    [KEY_KP7]=RETROK_KP7,
    [KEY_KP8]=RETROK_KP8,
    [KEY_KP9]=RETROK_KP9,
    [KEY_KPDOT]=RETROK_KP_PERIOD,
    [KEY_KPSLASH]=RETROK_KP_DIVIDE,
    [KEY_KPASTERISK]=RETROK_KP_MULTIPLY,
    [KEY_KPMINUS]=RETROK_KP_MINUS,
    [KEY_KPPLUS]=RETROK_KP_PLUS,
    [KEY_KPENTER]=RETROK_KP_ENTER,
    [KEY_KPEQUAL]=RETROK_KP_EQUALS,

    [KEY_UP]=RETROK_UP,
    [KEY_DOWN]=RETROK_DOWN,
    [KEY_RIGHT]=RETROK_RIGHT,
    [KEY_LEFT]=RETROK_LEFT,
    [KEY_INSERT]=RETROK_INSERT,
    [KEY_HOME]=RETROK_HOME,
    [KEY_END]=RETROK_END,
    [KEY_PAGEUP]=RETROK_PAGEUP,
    [KEY_PAGEDOWN]=RETROK_PAGEDOWN,

    [KEY_F1]=RETROK_F1,
    [KEY_F2]=RETROK_F2,
    [KEY_F3]=RETROK_F3,
    [KEY_F4]=RETROK_F4,
    [KEY_F5]=RETROK_F5,
    [KEY_F6]=RETROK_F6,
    [KEY_F7]=RETROK_F7,
    [KEY_F8]=RETROK_F8,
    [KEY_F9]=RETROK_F9,
    [KEY_F10]=RETROK_F10,
    [KEY_F11]=RETROK_F11,
    [KEY_F12]=RETROK_F12,
    [KEY_F13]=RETROK_F13,
    [KEY_F14]=RETROK_F14,
    [KEY_F15]=RETROK_F15,

    [KEY_NUMLOCK]=RETROK_NUMLOCK,
    [KEY_CAPSLOCK]=RETROK_CAPSLOCK,
    [KEY_SCROLLLOCK]=RETROK_SCROLLOCK,
    [KEY_RIGHTSHIFT]=RETROK_RSHIFT,
    [KEY_LEFTSHIFT]=RETROK_LSHIFT,
    [KEY_RIGHTCTRL]=RETROK_RCTRL,
    [KEY_LEFTCTRL]=RETROK_LCTRL,
    [KEY_RIGHTALT]=RETROK_RALT,
    [KEY_LEFTALT]=RETROK_LALT,
    [KEY_RIGHTMETA]=RETROK_RMETA,
    [KEY_LEFTMETA]=RETROK_LMETA,
    //[KEY_UNKNOWN]=RETROK_LSUPER,
    //[KEY_UNKNOWN]=RETROK_RSUPER,
    [KEY_MODE]=RETROK_MODE,
    [KEY_COMPOSE]=RETROK_COMPOSE,

    [KEY_HELP]=RETROK_HELP,
    [KEY_PRINT]=RETROK_PRINT,
    [KEY_SYSRQ]=RETROK_SYSREQ,
    [KEY_BREAK]=RETROK_BREAK,
    [KEY_MENU]=RETROK_MENU,
    [KEY_POWER]=RETROK_POWER,
    [KEY_EURO]=RETROK_EURO,
    [KEY_UNDO]=RETROK_UNDO,
    //[KEY_UNKNOWN]=RETROK_OEM_102,

};

int jsfd = -1, kbdfd = -1;
void init_input() {
    jsfd = open("/dev/input/by-path/platform-3f980000.usb-usb-0:1.3:1.0-event-joystick", O_RDONLY|O_NONBLOCK);
    kbdfd = open("/dev/input/event1", O_RDONLY|O_NONBLOCK);
    ioctl(kbdfd, EVIOCGRAB, 1);
//    rt_log("event: %d\n", kbdfd);
    keymap[KEY_ENTER] = RETROK_RETURN;
}

uint8_t keyboardstate[(RETROK_LAST + 7) >> 3];
int16_t joy_state[CONTROLS_MAX];
int mode_state;

void setbtn(int id, int v) {
    if (v) joy_state[JOYPAD_BUTTONS] |= 1 << id; else joy_state[JOYPAD_BUTTONS] &= ~(1 << id);
//    rt_log("%04X BTN\n", joy_state[0]);
}
void setkey(int id, int v) {
    if (v) keyboardstate[id >> 3] |= 1 << (id&7); else keyboardstate[id >> 3] &= ~(1 << (id&7));
}
int poll_input() {
    struct input_event ev;
    int ret = 0;
    while(read(jsfd, &ev, sizeof(ev)) > 0) {
//        rt_log("%d %d %d\n", ev.code, ev.type, ev.value);
        if (ev.type == EV_ABS) {
            if (ev.code == ABS_HAT0Y) setbtn(RETRO_DEVICE_ID_JOYPAD_UP, ev.value < 0);
            if (ev.code == ABS_HAT0Y) setbtn(RETRO_DEVICE_ID_JOYPAD_DOWN, ev.value > 0);
            if (ev.code == ABS_HAT0X) setbtn(RETRO_DEVICE_ID_JOYPAD_LEFT, ev.value < 0);
            if (ev.code == ABS_HAT0X) setbtn(RETRO_DEVICE_ID_JOYPAD_RIGHT, ev.value > 0);
            if (ev.code == ABS_Z) setbtn(RETRO_DEVICE_ID_JOYPAD_L2, ev.value > 0);
            if (ev.code == ABS_RZ) setbtn(RETRO_DEVICE_ID_JOYPAD_R2, ev.value > 0);
            if (ev.code == ABS_X) joy_state[1] = ev.value;
            if (ev.code == ABS_Y) joy_state[2] = ev.value;
            if (ev.code == ABS_RX) joy_state[3] = ev.value;
            if (ev.code == ABS_RY) joy_state[4] = ev.value;
        } else if (ev.type == EV_KEY) {
            if (ev.code == BTN_SOUTH) setbtn(RETRO_DEVICE_ID_JOYPAD_B, ev.value);
            if (ev.code == BTN_WEST) setbtn(RETRO_DEVICE_ID_JOYPAD_Y, ev.value);
            if (ev.code == BTN_SELECT) setbtn(RETRO_DEVICE_ID_JOYPAD_SELECT, ev.value);
            if (ev.code == BTN_START) setbtn(RETRO_DEVICE_ID_JOYPAD_START, ev.value);
            if (ev.code == BTN_EAST) setbtn(RETRO_DEVICE_ID_JOYPAD_A, ev.value);
            if (ev.code == BTN_NORTH) setbtn(RETRO_DEVICE_ID_JOYPAD_X, ev.value);
            if (ev.code == BTN_TL) setbtn(RETRO_DEVICE_ID_JOYPAD_L, ev.value);
            if (ev.code == BTN_TR) setbtn(RETRO_DEVICE_ID_JOYPAD_R, ev.value);
            if (ev.code == BTN_THUMBL) setbtn(RETRO_DEVICE_ID_JOYPAD_L3, ev.value);
            if (ev.code == BTN_THUMBR) setbtn(RETRO_DEVICE_ID_JOYPAD_R3, ev.value);
            if (ev.code == BTN_MODE) {
                if (!mode_state && ev.value) ret |= 32;
                mode_state = ev.value;
            }
        }
        ret |= 1;
    }
    while (read(kbdfd, &ev, sizeof(ev)) > 0)
    {
        if (ev.type == EV_KEY && ev.value < 2) {
//            rt_log("%d %d %d\n", ev.code, ev.type, ev.value);
            setkey(keymap[ev.code], ev.value);
        }
        ret |= 16;
    }
    
    return ret;
}


