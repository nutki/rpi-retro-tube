#ifndef MAININPUT_H
#define MAININPUT_H
#include <stdint.h>
#include "libretro.h"
#include "main.h"

extern void *input_handler_init(void);
extern uint32_t poll_devices(void);
extern int16_t *get_gamepad_state(int port);
extern uint8_t *get_keyboard_state(void);

#endif