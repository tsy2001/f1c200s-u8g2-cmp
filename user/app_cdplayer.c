#ifndef _POSIX_C_SOURCE
#define _POSIX_C_SOURCE 200809L
#endif

#include <pthread.h>
#include <linux/input.h>
#include <sys/times.h>
#include <string.h>
#include <errno.h>
#include <u8g2port.h>
#include "led.h"
#include "app_cdplayer.h"

// 光驱设备
#define OPTICAL_DEVICE "/dev/sr0"
// 声卡设备
#define PCM_DEVICE "hw:1,0"

led_t *mute_handle = NULL;
pthread_t cd_thread;
pthread_attr_t cd_thread_attr;
APP_CDIO app_cdio;

// 从dvdplayer.c引用的函数声明
int play_dvd(const char *cd_dev, APP_CDIO *pCDIO);
int reconfigure_alsa_rate(snd_pcm_t *pcm_handle, unsigned int sample_rate);

void apply_volume(void *buffer, size_t bytes, float *volume)
{
    int16_t *pcm_data = (int16_t *)buffer;
    size_t samples = bytes / 2; // 16-bit样本数

    if (*volume < 0.0f)
        *volume = 0.0f;
    if (*volume > 1.0f)
        *volume = 1.0f;

    for (size_t i = 0; i < samples; i++)
    {
        int32_t scaled = (int32_t)(pcm_data[i] * *volume);
        pcm_data[i] = (int16_t)(scaled > INT16_MAX ? INT16_MAX : (scaled < INT16_MIN ? INT16_MIN : scaled));
    }
}

uint8_t playCDTrack(APP_CDIO *pCDIO, track_t track)
{
    unsigned char buffer[BUFFER_SIZE];
    unsigned char tmpbuf[BUFFER_SIZE];

    // 检查是否有弹出光碟请求
    pthread_mutex_lock(&pCDIO->lock);
    if (pCDIO->ejct || pCDIO->eject_in_progress)
    {
        pthread_mutex_unlock(&pCDIO->lock);
        printf("playCDTrack: aborting because ejct in progress\n");
        return 1;
    }
    pthread_mutex_unlock(&pCDIO->lock);

    CdIo *cd = app_cdio_acquire_cdio();
    if (!cd)
    {
        printf("playCDTrack: cdio is NULL\n");
        return 1;
    }
    // 获取音轨起始和结束LSN
    lsn_t start_lsn = cdio_get_track_lsn(cd, track);
    lsn_t last_lsn = cdio_get_track_last_lsn(cd, track);
    app_cdio_release_cdio();

    printf("start_lsn %d\n", start_lsn);
    printf("last_lsn %d\n", last_lsn);

    if (start_lsn == CDIO_INVALID_LSN || last_lsn == CDIO_INVALID_LSN)
    {
        printf("Invalid track LSN\n");
        return 1;
    }

    lsn_t num_sectors = last_lsn - start_lsn + 1; // 计算总扇区数
    pCDIO->total_lsn = num_sectors;
    pCDIO->now_lsn = 0;

    while (num_sectors > 0)
    {
        // 检查是否有弹出光碟请求
        pthread_mutex_lock(&pCDIO->lock);
        if (pCDIO->ejct || pCDIO->eject_in_progress)
        {
            pthread_mutex_unlock(&pCDIO->lock);
            printf("playCDTrack: stopping read loop due to ejct\n");
            break;
        }
        pthread_mutex_unlock(&pCDIO->lock);

        // 使用libcdio读取音轨LSN
        CdIo *cd_read = app_cdio_acquire_cdio();
        if (!cd_read)
        {
            printf("playCDTrack: cdio is NULL during read\n");
            return 1;
        }
        pCDIO->scsi_read_flag = 1;
        driver_return_code_t rc = cdio_read_audio_sector(cd_read, buffer, start_lsn);
        pCDIO->scsi_read_flag = 0;
        if (rc != DRIVER_OP_SUCCESS)
        {
            printf("cdio_read_audio_sector failed rc=%d at lsn=%d\n", rc, (int)start_lsn);
        }
        app_cdio_release_cdio();

        if (rc != DRIVER_OP_SUCCESS)
        {
            printf("Error reading audio sectors\n");
            return 1;
        }

        // 暂存原子量
        float vol_snapshot;
        pthread_mutex_lock(&pCDIO->lock);
        vol_snapshot = pCDIO->volume;
        pthread_mutex_unlock(&pCDIO->lock);
        // 软件控制音量
        apply_volume(buffer, CDIO_CD_FRAMESIZE_RAW, &vol_snapshot);

        int frames_written = snd_pcm_writei(pCDIO->pcm_handle, buffer, (CDIO_CD_FRAMESIZE_RAW / 4)); // 16-bit stereo
        if (frames_written < 0)
        {
            // 尝试恢复
            int err = snd_pcm_recover(pCDIO->pcm_handle, frames_written, 0);
            if (err < 0)
            {
                // 恢复失败，进入准备状态
                snd_pcm_prepare(pCDIO->pcm_handle);
            }
        }
        else
        {
            pthread_mutex_lock(&pCDIO->lock);
            pCDIO->now_lsn += 1;
            pthread_mutex_unlock(&pCDIO->lock);

            start_lsn += 1;
            num_sectors -= 1;
        }

        // 暂存原子量
        pthread_mutex_lock(&pCDIO->lock);
        uint8_t next = pCDIO->next;
        uint8_t prev = pCDIO->prev;
        uint8_t ejct = pCDIO->ejct;
        uint16_t manual_track = pCDIO->manual_track;
        pthread_mutex_unlock(&pCDIO->lock);

        if (next || prev || ejct || manual_track != 0)
        {
            if (manual_track > pCDIO->total_tracks || manual_track + 1 == pCDIO->now_tracks)
            {
                // 无效跳转，继续播放当前音轨
                pCDIO->manual_track = 0;
                sleep(1);
                continue;
            }
            
            int st = snd_pcm_state(pCDIO->pcm_handle);
            if (st == SND_PCM_STATE_RUNNING || st == SND_PCM_STATE_XRUN ||
                st == SND_PCM_STATE_SUSPENDED || st == SND_PCM_STATE_DISCONNECTED)
            {
                printf("Draining playback\n");
                snd_pcm_drain(pCDIO->pcm_handle);  // 播放完buffer缓冲区
                // snd_pcm_drop(pCDIO->pcm_handle); // 立即清空boffer缓冲区
                snd_pcm_prepare(pCDIO->pcm_handle); // 进入准备状态
            }
            
            return 0;
        }

        // 暂停播放处理
        while (1)
        {
            pthread_mutex_lock(&pCDIO->lock);
            uint8_t stop_flag = pCDIO->stop;
            pthread_mutex_unlock(&pCDIO->lock);
            if (!stop_flag)
            {
                app_cdio_set_mute(false);
                break;
            }
            else 
            {
                app_cdio_set_mute(true);
            }
            sleep(1);
        }
    }

    printf("play done\n");

    return 0;
}

void readCDInfo(APP_CDIO *pCDIO)
{
    pCDIO->cdtext = NULL;
    reconfigure_alsa_rate(app_cdio.pcm_handle, 44100);
    int attempts = 0;   // 尝试读取CD Text次数
    while (1)
    {
        CdIo *cd = app_cdio_acquire_cdio();
        if (!cd)
        {
            pthread_mutex_lock(&pCDIO->lock);
            pCDIO->cdda_ready_flag = 0;
            pthread_mutex_unlock(&pCDIO->lock);
            printf("CD handle is NULL when reading track info\n");
            sleep(2);
            if (++attempts > 5)
                return; // 放弃读取
            continue;
        }

        // 读取CD音轨总数
        pCDIO->now_tracks = cdio_get_first_track_num(cd);
        pCDIO->total_tracks = cdio_get_num_tracks(cd);
        app_cdio_release_cdio();

        if (pCDIO->now_tracks == CDIO_INVALID_TRACK || pCDIO->total_tracks == CDIO_INVALID_TRACK)
        {
            pthread_mutex_lock(&pCDIO->lock);
            pCDIO->cdda_ready_flag = 0;
            pthread_mutex_unlock(&pCDIO->lock);
            printf("Failed to get CD track info\n");
            sleep(2);
            if (++attempts > 5)
                return;
            continue;
        }
        break;
    }

    pthread_mutex_lock(&pCDIO->lock);
    printf("CD-ROM Track List (%i - %i)\n", pCDIO->now_tracks, pCDIO->total_tracks);
    printf("  #:  LSN\n");
    pCDIO->cdda_ready_flag = 1; // 获取到了CD音轨信息
    int j, i = pCDIO->now_tracks;
    pthread_mutex_unlock(&pCDIO->lock);

    CdIo *cd_for_lsns = app_cdio_acquire_cdio();

    for (j = 0; j < pCDIO->total_tracks; i++, j++)
    {
        lsn_t lsn = CDIO_INVALID_LSN;
        if (cd_for_lsns)
            lsn = cdio_get_track_lsn(cd_for_lsns, i);
        if (CDIO_INVALID_LSN != lsn)
            printf("%3d: %06d\n", (int)i, lsn);
    }

    lsn_t leadout = CDIO_INVALID_LSN;
    if (cd_for_lsns) leadout = cdio_get_track_lsn(cd_for_lsns, CDIO_CDROM_LEADOUT_TRACK);

    printf("%3X: %06d  leadout\n", CDIO_CDROM_LEADOUT_TRACK, leadout);

    if (cd_for_lsns) app_cdio_release_cdio();

    // read_text:
    pCDIO->album_ready_flag = 0;
    CdIo *cdtext_cd = app_cdio_acquire_cdio();
    pCDIO->cdtext = cdtext_cd ? cdio_get_cdtext(cdtext_cd) : NULL;
    if (pCDIO->cdtext)
    {
        // 似乎没用，buildroot编译libcdio时只支持英文解码
        int cd_text_language = cdtext_get_language(pCDIO->cdtext);
        printf("cd-text language index: %d, language: %s\n", cd_text_language, cdtext_lang2str(cd_text_language));

        track_t i_last_track = pCDIO->now_tracks + pCDIO->total_tracks;

        if (cdtext_select_language(pCDIO->cdtext, cd_text_language))
        {
            printf("%s selected\n", cdtext_lang2str(cd_text_language));
        }
        else
        {
            printf("'%s' is not available, Using '%s'\n",
                   cdtext_lang2str(cd_text_language),
                   cdtext_lang2str(cdtext_get_language(pCDIO->cdtext)));
        }

        // 清除之前分配的内存
        printf("free previous album info if any\n");
        if (pCDIO->album_info != NULL)
        {
            free(pCDIO->album_info);
            pCDIO->album_info = NULL;
        }

        if (pCDIO->album_artist != NULL)
        {
            free(pCDIO->album_artist);
            pCDIO->album_artist = NULL;
        }

        // 分配新的内存，album_info：歌曲名，album_artist：歌手名
        printf("allocate album info/artist arrays\n");
        pCDIO->album_info = calloc((i_last_track + 1), sizeof(char *));
        pCDIO->album_artist = calloc((i_last_track + 1), sizeof(char *));

        if (!pCDIO->album_info || !pCDIO->album_artist)
        {
            printf("Failed to allocate memory for album info/artist\n");
            if (pCDIO->album_info) { free(pCDIO->album_info); pCDIO->album_info = NULL; }
            if (pCDIO->album_artist) { free(pCDIO->album_artist); pCDIO->album_artist = NULL; }
        }
        else
        {
            // 为了方便ui线程读取，第0个作为dummy占位
            const char *album_title = cdtext_get_const(pCDIO->cdtext, CDTEXT_FIELD_TITLE, 0);
            pCDIO->album_info[0] = album_title ? strdup(album_title) : NULL;
            pCDIO->album_artist[0] = album_title ? strdup(album_title) : NULL;

            for (track_t ii = 1; ii <= i_last_track; ii++)
            {
                const char *track_title = cdtext_get_const(pCDIO->cdtext, CDTEXT_FIELD_TITLE, pCDIO->now_tracks + ii - 1);
                const char *track_artist = cdtext_get_const(pCDIO->cdtext, CDTEXT_FIELD_PERFORMER, pCDIO->now_tracks + ii - 1);

                pCDIO->album_info[ii] = track_title ? strdup(track_title) : NULL;
                pCDIO->album_artist[ii] = track_artist ? strdup(track_artist) : NULL;
            }
        }
        // 都拷贝完毕了，可以释放cdtext
        cdtext_destroy(pCDIO->cdtext);

        if (pCDIO->album_info)
        {
            // 打印cd text信息
            printf("Album: %s\n", pCDIO->album_info[0] ? pCDIO->album_info[0] : "Unknown");
            for (track_t i = 1; i < i_last_track; i++)
            {
                printf("%d: %s - %s\n", i, pCDIO->album_artist[i] ? pCDIO->album_artist[i] : "Unknown", pCDIO->album_info[i] ? pCDIO->album_info[i] : "Unknown");
            }
            // 使用nero或ONES软件刻录的光盘，title一般都是Music或Audio，中文编码乱码，所以不要显示cd text
            if (pCDIO->album_info[0] != NULL && (strstr(pCDIO->album_info[0], "Music") || strstr(pCDIO->album_info[0], "Audio")))
            {
                pCDIO->album_ready_flag = 0;
            }
            else
            {
                pCDIO->album_ready_flag = 1;
            }
        }
        else
        {
            pCDIO->album_ready_flag = 0;
            printf("album_info got null pointer\n");
        }
    }
    else
    {
        pCDIO->album_ready_flag = 0;
        if (pCDIO->album_info != NULL)
        {
            free(pCDIO->album_info);
        }
        printf("No CD text information available\n");
    }
    if (cdtext_cd) app_cdio_release_cdio();

    // 进入播放循环
    while (1)
    {
        pthread_mutex_lock(&pCDIO->lock);
        if (pCDIO->next)    // 下一首处理
        {
            pCDIO->now_tracks += 1;
            if (pCDIO->now_tracks > pCDIO->total_tracks + 1)    // 播完了回到第一手
            {
                pCDIO->now_tracks = 2;
            }
            pCDIO->next = 0;
        }
        else if (pCDIO->prev)
        {
            if (pCDIO->now_tracks <= 2)
            {
                pCDIO->now_tracks = pCDIO->total_tracks + 1;
            }
            else
            {
                pCDIO->now_tracks -= 1;
            }
            pCDIO->prev = 0;
        }
        else if (pCDIO->manual_track != 0)  // 收到遥控器指令，手动跳转曲目
        {
            track_t requested_track = pCDIO->manual_track;
            pCDIO->manual_track = 0;
            if (requested_track >= 1 && requested_track <= pCDIO->total_tracks)
            {
                pCDIO->now_tracks = requested_track + 1;
            }
        }
        else
        {   // 没有切歌操作，继续下一首
            pCDIO->now_tracks += 1;
            if (pCDIO->now_tracks > pCDIO->total_tracks + 1)
            {
                pCDIO->now_tracks = 2;
            }
        }
        pthread_mutex_unlock(&pCDIO->lock);

        uint8_t res = playCDTrack(pCDIO, pCDIO->now_tracks - 1); // 开始播放
        if (res)
        {
            printf("Play CD track error\n");
            return;
        }

        pthread_mutex_lock(&pCDIO->lock);
        if (pCDIO->ejct)
        {   // 如果需要弹出光碟，直接结束函数
            pthread_mutex_unlock(&pCDIO->lock);
            return;
        }
        pthread_mutex_unlock(&pCDIO->lock);
    }
}

void *cd_player_thread_entry(void *arg)
{
    app_cdio.volume = 0.2f;
    printf("try to open alsa device\n");
    if (snd_pcm_open(&app_cdio.pcm_handle, PCM_DEVICE, SND_PCM_STREAM_PLAYBACK, 0) < 0)
    {
        printf("Failed to open ALSA device\n");
        return NULL;
    }
    // reconfigure_alsa_rate(app_cdio.pcm_handle, 44100, 16); // 放到readCDInfo里设置了
    while (1)
    {
        pthread_mutex_lock(&app_cdio.lock);
        app_cdio.cdda_ready_flag = 0;
        pthread_mutex_unlock(&app_cdio.lock);

        CdIo *cd = cdio_open(OPTICAL_DEVICE, DRIVER_DEVICE);
        if (!cd)
        {
            printf("Failed to open CD \n");
            sleep(2);
            continue;
        }

        app_cdio.disc_mode = cdio_get_discmode(cd);
        if (app_cdio.disc_mode == CDIO_DISC_MODE_ERROR) 
        {
            printf("Waiting for disc...\n");
            cdio_destroy(cd);
            sleep(2);
            continue;
        }

        // 保证原子操作
        pthread_mutex_lock(&app_cdio.lock);
        app_cdio.cdio = cd;
        pthread_mutex_unlock(&app_cdio.lock);
        
        printf("Disc mode: %d\n", app_cdio.disc_mode);
        if (app_cdio.disc_mode == CDIO_DISC_MODE_CD_DA)
        {
            printf("This is a CD-DA disc\n");
            readCDInfo(&app_cdio); // 这是CD-DA光碟，播放CD音轨
        }
        else if (app_cdio.disc_mode == CDIO_DISC_MODE_CD_DATA ||
                app_cdio.disc_mode == CDIO_DISC_MODE_DVD_R ||
                app_cdio.disc_mode == CDIO_DISC_MODE_DVD_RW ||
                app_cdio.disc_mode == CDIO_DISC_MODE_DVD_ROM)
        {
            printf("This is a CD/DVD-ROM disc\n");
            cdio_destroy(cd);
            pthread_mutex_lock(&app_cdio.lock);
            app_cdio.cdio = NULL;
            app_cdio.album_ready_flag = 0;
            pthread_mutex_unlock(&app_cdio.lock);
            play_dvd("/dev/sr0", &app_cdio); // 这是个DVD/CD-ROM光碟，当作存储器挂载播放
        }

        pthread_mutex_lock(&app_cdio.lock);
        int eject_requested = app_cdio.ejct;
        pthread_mutex_unlock(&app_cdio.lock);

        if (eject_requested) // 光碟弹出操作
        {
            pthread_mutex_lock(&app_cdio.lock);
            app_cdio.eject_in_progress = 1; // 告知ui线程
            app_cdio.cdda_ready_flag = 0;
            pthread_mutex_unlock(&app_cdio.lock);

            // 通知所有线程
            pthread_cond_broadcast(&app_cdio.cond);

            // 等待所有线程都不再使用*pcdio
            struct timespec ts;
            clock_gettime(CLOCK_REALTIME, &ts);
            ts.tv_sec += 2; // 等2秒钟
            pthread_mutex_lock(&app_cdio.lock);
            while (app_cdio.cdio_refcount > 0)
            {
                if (pthread_cond_timedwait(&app_cdio.cond, &app_cdio.lock, &ts) == ETIMEDOUT)
                    break;
            }
            pthread_mutex_unlock(&app_cdio.lock);

            CdIo *cdio_to_destroy = NULL;
            pthread_mutex_lock(&app_cdio.lock);
            app_cdio.album_ready_flag = 0;
            app_cdio.ejct = 0;
            app_cdio.eject_in_progress = 0;
            cdio_to_destroy = app_cdio.cdio;
            app_cdio.cdio = NULL;
            pthread_mutex_unlock(&app_cdio.lock);

            if (cdio_to_destroy)
            {
                pthread_mutex_lock(&app_cdio.lock);
                while (app_cdio.cdio_refcount > 0)
                {
                    pthread_cond_wait(&app_cdio.cond, &app_cdio.lock);
                }
                pthread_mutex_unlock(&app_cdio.lock);
                // cdio_destroy(cdio_to_destroy);
            }

            // 可以安全弹出光碟了
            printf("Ejecting media (cd=%p)\n", (void *)cd);
            cdio_eject_media_drive(OPTICAL_DEVICE);
            printf("Eject complete\n");
        }
        else if(app_cdio.disc_mode == CDIO_DISC_MODE_CD_DA)
        {   // 如果是CD—DA光碟，走到这一部也没有弹出请求，那么说明音轨读取错误，关闭cdio从头再来一遍
            cdio_close_tray(OPTICAL_DEVICE, NULL);
            CdIo *cdio_to_destroy = NULL;
            pthread_mutex_lock(&app_cdio.lock);
            app_cdio.album_ready_flag = 0;
            cdio_to_destroy = app_cdio.cdio;
            app_cdio.cdio = NULL;
            pthread_mutex_unlock(&app_cdio.lock);

            if (cdio_to_destroy)
            {
                pthread_mutex_lock(&app_cdio.lock);
                while (app_cdio.cdio_refcount > 0)
                {
                    pthread_cond_wait(&app_cdio.cond, &app_cdio.lock);
                }
                pthread_mutex_unlock(&app_cdio.lock);
                // cdio_destroy(cdio_to_destroy);
            }
        }

        sleep(2);
    }

    return NULL;
}

void app_cdplayer_start(void)
{
    int err = 0;
    mute_handle = led_new();
    if (mute_handle == NULL)
    {
        printf("Failed to create mute handle\n");
        return;
    }

    err = led_open(mute_handle, "mute");
    if (err != 0)
    {
        printf("Failed to open LED device\n");
        led_free(mute_handle);
        mute_handle = NULL;
        return;
    }

    pthread_attr_init(&cd_thread_attr);

    struct sched_param param;
    param.sched_priority = 90;

    pthread_attr_setschedpolicy(&cd_thread_attr, SCHED_FIFO);   // FIFO实时调度策略
    pthread_attr_setschedparam(&cd_thread_attr, &param);
    pthread_attr_setinheritsched(&cd_thread_attr, PTHREAD_EXPLICIT_SCHED);

    pthread_create(&cd_thread, &cd_thread_attr, cd_player_thread_entry, NULL);
    // pthread_create(&cd_thread, NULL, cd_player_thread_entry, NULL); // 默认的调度策略，不能保证时实行
}

void app_cdplayer_wait(void)
{
    led_free(mute_handle);
    pthread_join(cd_thread, NULL);
}

// 线程同步 线程安全操作
void app_cdio_set_next(void)
{
    pthread_mutex_lock(&app_cdio.lock);
    app_cdio.next = 1;
    pthread_cond_signal(&app_cdio.cond);
    pthread_mutex_unlock(&app_cdio.lock);
}

void app_cdio_set_prev(void)
{
    pthread_mutex_lock(&app_cdio.lock);
    app_cdio.prev = 1;
    pthread_cond_signal(&app_cdio.cond);
    pthread_mutex_unlock(&app_cdio.lock);
}

void app_cdio_set_manual_track(uint16_t track)
{
    pthread_mutex_lock(&app_cdio.lock);
    app_cdio.manual_track = track;
    pthread_cond_signal(&app_cdio.cond);
    pthread_mutex_unlock(&app_cdio.lock);
}

void app_cdio_set_eject(void)
{
    pthread_mutex_lock(&app_cdio.lock);
    app_cdio.ejct = 1;
    pthread_cond_broadcast(&app_cdio.cond);
    pthread_mutex_unlock(&app_cdio.lock);
}

CdIo *app_cdio_acquire_cdio(void)
{
    pthread_mutex_lock(&app_cdio.lock);
    CdIo *cd = app_cdio.cdio;
    if (cd && !app_cdio.eject_in_progress)
        app_cdio.cdio_refcount++;
    else
        cd = NULL;
    pthread_mutex_unlock(&app_cdio.lock);
    return cd;
}

void app_cdio_release_cdio(void)
{
    pthread_mutex_lock(&app_cdio.lock);
    if (app_cdio.cdio_refcount > 0)
        app_cdio.cdio_refcount--;
    if (app_cdio.cdio_refcount == 0)
        pthread_cond_broadcast(&app_cdio.cond);
    pthread_mutex_unlock(&app_cdio.lock);
}

void app_cdio_toggle_stop(void)
{
    pthread_mutex_lock(&app_cdio.lock);
    app_cdio.stop = !app_cdio.stop;
    pthread_cond_broadcast(&app_cdio.cond);
    pthread_mutex_unlock(&app_cdio.lock);
}

void app_cdio_adjust_volume(float delta)
{
    pthread_mutex_lock(&app_cdio.lock);
    app_cdio.volume += delta;
    if (app_cdio.volume < 0.0f)
        app_cdio.volume = 0.0f;
    if (app_cdio.volume > 1.0f)
        app_cdio.volume = 1.0f;
    pthread_cond_broadcast(&app_cdio.cond);
    pthread_mutex_unlock(&app_cdio.lock);
}

void app_cdio_set_mute(bool mute)
{
    if (mute_handle == NULL)
        return;

    if (mute)
    {
        led_write(mute_handle, 0);
    }
    else
    {
        led_write(mute_handle, 1);
    }
}
