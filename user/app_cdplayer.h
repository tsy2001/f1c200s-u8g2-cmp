#ifndef _APP_CDPLAYER_H_
#define _APP_CDPLAYER_H_

#include <stdio.h>
#include <stdint.h>
#include <stdbool.h>
#include <sys/types.h>
#include <cdio/cdio.h>
#include <cdio/mmc.h>
#include <alsa/asoundlib.h>
#include <pthread.h>
#include "app_u8g2.h"

#define BUFFER_SIZE (CDIO_CD_FRAMESIZE_RAW * 2)
#define ONE_ALBUM_LENGTH 100

typedef enum {
    APP_MODE_MENU = 0,
    APP_MODE_DISC,
    APP_MODE_SD,
    APP_MODE_PC,
} APP_MODE;

typedef enum {
    APP_MENU_DISC = 0,
    APP_MENU_SD,
    APP_MENU_PC,
    APP_MENU_COUNT,
} APP_MENU_ITEM;

typedef struct {
    CdIo *cdio;
    cdtext_t *cdtext;
    discmode_t disc_mode;
    snd_pcm_hw_params_t *pcm_params;
    snd_pcm_t *pcm_handle;

    lsn_t total_lsn;    
    lsn_t now_lsn;

    uint16_t now_tracks;
    uint16_t total_tracks;
    uint16_t manual_track;

    char **album_info;
    char **album_artist;
    uint16_t album_entries;
    char now_title[ONE_ALBUM_LENGTH];

    uint8_t album_ready_flag;
    uint8_t cdda_ready_flag;
    uint8_t scsi_read_flag;

    uint8_t next, prev, stop, ejct;
    float volume;
    pthread_mutex_t lock;
    pthread_cond_t cond;
    
    uint8_t eject_in_progress; /* internal: set while eject handling waits for in-flight ops to finish */
    int cdio_refcount; /* number of threads currently using cdio */

    uint8_t usb_pc_mode; /* 1: USB peripheral serial shell mode, 0: normal host mode */
    pid_t usb_shell_pid; /* child shell pid bound to /dev/ttyGS0 */

    APP_MODE app_mode;
    APP_MENU_ITEM menu_index;
    uint8_t return_to_menu;
} APP_CDIO;

void *cd_player_thread_entry(void *);
void app_cdplayer_start(void);
void app_cdplayer_wait(void);
void apply_volume(void *buffer, size_t bytes, float *volume);

/* Thread-safe setters used by other threads (button handlers etc.) */
void app_cdio_set_next(void);
void app_cdio_set_prev(void);
void app_cdio_set_manual_track(uint16_t track);
void app_cdio_set_eject(void);
void app_cdio_toggle_stop(void);
void app_cdio_adjust_volume(float delta);
void app_cdio_set_mute(bool mute);
void app_cdio_menu_prev(void);
void app_cdio_menu_next(void);
void app_cdio_menu_confirm(void);
void app_cdio_return_to_menu(void);
APP_MODE app_cdio_get_mode(void);
/* cdio usage reference counting (thread-safe) */
CdIo *app_cdio_acquire_cdio(void);
void app_cdio_release_cdio(void);
#endif
