#include <math.h>
#include <stdio.h>
#include <string.h>
#include <fcntl.h>
#include <unistd.h>
#include <signal.h>
#include <errno.h>
#include <linux/input.h>
#include <pthread.h>
#include <u8g2/u8g2.h>
#include "u8g2_fonts.h"
#include "led.h"
#include "app_u8g2.h"
#include "app_cdplayer.h"
#include "logo.h"

#define I2C_BUS 2

static void sleep_ms_local(unsigned int ms)
{
    struct timespec req;

    req.tv_sec = ms / 1000;
    req.tv_nsec = (long)(ms % 1000) * 1000000L;
    while (nanosleep(&req, &req) < 0 && errno == EINTR)
        ;
}

u8g2_t u8g2;
pthread_t ui_thread;
char oled_buffer[ONE_ALBUM_LENGTH * 2] = {0};
uint8_t album_ready_flag;
extern APP_CDIO app_cdio;

void app_u8g2_start(void)
{
    // 线程同步用,需要在其他线程启动前，最先初始化
    pthread_mutex_init(&app_cdio.lock, NULL);
    pthread_cond_init(&app_cdio.cond, NULL);

    pthread_create(&ui_thread, NULL, ui_thread_entry, NULL);
}

void app_u8g2_wait(void)
{
    pthread_join(ui_thread, NULL);
}

void *ui_thread_entry(void *arg)
{
    libu8g2_Setup(&u8g2, I2C_BUS);

    while (1)
    {
        u8g2_cdplayer();
        if (!album_ready_flag)
            sleep_ms_local(50);
        else
            sleep_ms_local(10);
    }

    libu8g2_Done(&u8g2);
    return NULL;
}

void u8g2_scroll_text(char *text, uint8_t y, uint16_t screen_width)
{
    static int16_t offset = 0;
    static char last_text[ONE_ALBUM_LENGTH * 2] = {0};
    if (strncmp(last_text, text, sizeof(last_text)) != 0)
    {
        offset = 0;
        snprintf(last_text, sizeof(last_text), "%s", text);
    }
    int16_t text_width = u8g2_GetUTF8Width(&u8g2, text);

    if (text_width <= screen_width)
    {
        int16_t x = (screen_width - text_width) / 2;
        u8g2_DrawUTF8(&u8g2, x, y, text);
    }
    else
    {
        int16_t gap = u8g2_GetUTF8Width(&u8g2, "  ");
        int16_t period = text_width + gap;
        u8g2_DrawUTF8(&u8g2, offset, y, text);
        u8g2_DrawUTF8(&u8g2, offset + period, y, text);
        offset--;
        if (offset <= -period)
        {
            offset = 0;
        }
    }
}

void u8g2_cdplayer(void)
{
    u8g2_ClearBuffer(&u8g2);

    // 暂存原子量
    uint8_t cdda_ready_flag, eject_flag;
    lsn_t now_lsn;
    lsn_t total_lsn;
    uint16_t now_tracks;
    uint16_t total_tracks;
    float volume;
    uint8_t stop_flag, next_flag, prev_flag;
    uint16_t album_entries;
    APP_MODE app_mode;
    APP_MENU_ITEM menu_index;
    char artist_buf[ONE_ALBUM_LENGTH];
    char title_buf[ONE_ALBUM_LENGTH];
    char file_buf[ONE_ALBUM_LENGTH];

    pthread_mutex_lock(&app_cdio.lock);
    cdda_ready_flag = app_cdio.cdda_ready_flag;
    eject_flag = app_cdio.eject_in_progress;
    now_lsn = app_cdio.now_lsn;
    total_lsn = app_cdio.total_lsn;
    now_tracks = app_cdio.now_tracks;
    total_tracks = app_cdio.total_tracks;
    album_ready_flag = app_cdio.album_ready_flag;
    volume = app_cdio.volume;
    stop_flag = app_cdio.stop;
    next_flag = app_cdio.next;
    prev_flag = app_cdio.prev;
    app_mode = app_cdio.app_mode;
    menu_index = app_cdio.menu_index;
    album_entries = app_cdio.album_entries;
    // 搬运专辑信息到临时缓区
    artist_buf[0] = '\0';
    title_buf[0] = '\0';
    file_buf[0] = '\0';
    if (app_cdio.album_info != NULL || app_cdio.album_artist != NULL)
    {
        int idx = (int)now_tracks - 1;
        // artist
        if (app_cdio.album_artist && idx >= 0 && idx < album_entries && app_cdio.album_artist[idx])
        {
            strncpy(artist_buf, app_cdio.album_artist[idx], ONE_ALBUM_LENGTH - 1);
        }
        else if (app_cdio.album_info && album_entries > 0 && app_cdio.album_info[0])
        {
            strncpy(artist_buf, app_cdio.album_info[0], ONE_ALBUM_LENGTH - 1);
        }
        // title
        if (app_cdio.album_info && idx >= 0 && idx < album_entries && app_cdio.album_info[idx])
        {
            strncpy(title_buf, app_cdio.album_info[idx], ONE_ALBUM_LENGTH - 1);
        }
        // 结束符
        artist_buf[ONE_ALBUM_LENGTH - 1] = '\0';
        title_buf[ONE_ALBUM_LENGTH - 1] = '\0';
    }
    if (app_cdio.now_title[0] != '\0')
    {
        strncpy(file_buf, app_cdio.now_title, ONE_ALBUM_LENGTH - 1);
        file_buf[ONE_ALBUM_LENGTH - 1] = '\0';
    }
    pthread_mutex_unlock(&app_cdio.lock);

    if (app_mode == APP_MODE_MENU)
    {
        const char *items[APP_MENU_COUNT] = {"Disc", "SD", "UAC", "MTP"};

        u8g2_DrawXBM(&u8g2, 0, 0, 64, 64, bmp);
        u8g2_SetFont(&u8g2, u8g2_font_spleen8x16_mf);
        for (int i = 0; i < APP_MENU_COUNT; i++)
        {
            int y = 13 + i * 15;
            if ((APP_MENU_ITEM)i == menu_index)
                u8g2_DrawStr(&u8g2, 72, y, ">");
            u8g2_DrawStr(&u8g2, 88, y, items[i]);
        }
    }
    else if (eject_flag) 
    {
        u8g2_DrawXBM(&u8g2, 0, 0, 64, 64, bmp);

        u8g2_SetFont(&u8g2, u8g2_font_spleen8x16_mf);
        u8g2_DrawStr(&u8g2, 78, 35, "Disc");
        u8g2_DrawStr(&u8g2, 78, 50, "Eject");
    }
    else if (cdda_ready_flag)
    {
        u8g2_SetFont(&u8g2, u8g2_font_streamline_all_t);
        if (now_lsn % 100 > 15)
        {
            u8g2_DrawGlyph(&u8g2, 107, 21, 460); // 光盘
        }

        u8g2_DrawGlyph(&u8g2, 0, 21, 484);  // 喇叭
        u8g2_DrawGlyph(&u8g2, 42, 21, 567); // 专辑

        u8g2_SetFont(&u8g2, u8g2_font_misans_thin_9_ascii);
        sprintf(oled_buffer, "%02d", (int)(volume*100));
        u8g2_DrawStr(&u8g2, 23, 15, oled_buffer); // 音量
        sprintf(oled_buffer, "%02d/%02d", now_tracks - 1, total_tracks);
        u8g2_DrawStr(&u8g2, 65, 15, oled_buffer);
        if (album_ready_flag && (artist_buf[0] != '\0' || title_buf[0] != '\0'))
        {
            // artist_buf是否为空，空则显示Unknown(todo：个别情况异常需修复)
            const char *artist = (artist_buf[0] != '\0') ? artist_buf : "Unknown";
            const char *title = (title_buf[0] != '\0') ? title_buf : "Unknown";
            sprintf(oled_buffer, "%s-%s", artist, title);
            u8g2_SetFont(&u8g2, u8g2_font_spleen12x24_mf);
            u8g2_scroll_text(oled_buffer, 42, 128);
        }
        else if (file_buf[0] != '\0')
        {
            u8g2_SetFont(&u8g2, u8g2_font_misans_light_16_cjk);
            u8g2_scroll_text(file_buf, 42, 128);
        }
        else
        {
            // 没有专辑信息时显示Track(xx)
            u8g2_SetFont(&u8g2, u8g2_font_spleen12x24_mf);
            sprintf(oled_buffer, "Track%02d", now_tracks - 1);
            u8g2_DrawStr(&u8g2, 26, 42, oled_buffer);
        }

        u8g2_SetFont(&u8g2, u8g2_font_open_iconic_all_2x_t);
        if (stop_flag)
        {
            u8g2_DrawGlyph(&u8g2, 0, 63, 210); // 暂停
        }
        else if (prev_flag)
        {
            u8g2_DrawGlyph(&u8g2, 0, 63, 215); // 左进
        }
        else if (next_flag || now_lsn == total_lsn)
        {
            u8g2_DrawGlyph(&u8g2, 0, 63, 216); // 右进
        }
        else
        {
            u8g2_DrawGlyph(&u8g2, 0, 63, 211); // 播放
        }

        u8g2_DrawFrame(&u8g2, 18, 50, 110, 12); // 进度条
        uint16_t width = 0;
        if (total_lsn > 0)
            width = ((float)now_lsn / (float)total_lsn) * 106.0f;
        u8g2_DrawBox(&u8g2, 20, 52, width, 8);
    }
    else
    {
        static uint8_t stat = 0;
        static uint16_t counter = 0;
        counter++;
        if (counter >= 10)
        {
            counter = 0;
            stat = !stat;
        }
        
        u8g2_DrawXBM(&u8g2, 0, 0, 64, 64, bmp);

        u8g2_SetFont(&u8g2, u8g2_font_spleen8x16_mf);
        if (app_mode == APP_MODE_UAC)
        {
            u8g2_DrawStr(&u8g2, 78, 35, "UAC");
            u8g2_DrawStr(&u8g2, 78, 50, "Mode");
        }
        else if (app_mode == APP_MODE_MTP)
        {
            u8g2_DrawStr(&u8g2, 78, 35, "MTP");
            u8g2_DrawStr(&u8g2, 78, 50, "Mode");
        }
        else
        {
            u8g2_DrawStr(&u8g2, 78, 35, "No");
            u8g2_DrawStr(&u8g2, 78, 50, "Disc");
            if (stat)
            {
                u8g2_SetFont(&u8g2, u8g2_font_open_iconic_all_2x_t);
                u8g2_DrawGlyph(&u8g2, 107, 21, 197);
            }
        }
    }

    u8g2_SendBuffer(&u8g2);
}
