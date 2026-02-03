#ifndef _APP_u8g2_h_
#define _APP_u8g2_h_


#include <stdio.h>
#include <stdint.h>

enum _DIS_PAGE {
    U8G2_PAGE_NO_DISK = 0,
    U8G2_PAGE_PLAYING_WITH_INFO,
    U8G2_PAGE_PLAYING_WITHOUT_INFO,
    U8G2_PAGE_EJECTING,
    U8G2_PAGE_MAX,
};
typedef enum _DIS_PAGE U8G2_DIS_PAGE;

void app_u8g2_start(void);
void u8g2_cdplayer(void);
void app_u8g2_wait(void);
void *ui_thread_entry(void *arg);


#endif

