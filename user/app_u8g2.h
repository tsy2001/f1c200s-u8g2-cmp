#ifndef _APP_u8g2_h_
#define _APP_u8g2_h_


#include <stdio.h>
#include <stdint.h>

void app_u8g2_start(void);
void u8g2_cdplayer(void);
void app_u8g2_wait(void);
void *ui_thread_entry(void *arg);


#endif

