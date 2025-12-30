#ifndef BEEP_H
#define BEEP_H
#include <stdio.h>
#include <stdint.h>

int beep_init(void);
void beep_deinit(void);
void beep_play_notify(void);
void beep_play_music(void);
void beep_play_button(uint8_t press);
void play_test(void);

#endif // BEEP_H
