#ifndef MAININPUT_H
#define MAININPUT_H
#include <stdint.h>
#include "libretro.h"
#include "main.h"

uint8_t keyboardstate[(RETROK_LAST + 7) >> 3];
int16_t joy_state[CONTROLS_MAX];
int mode_state;

void init_input();
int poll_input();

#endif