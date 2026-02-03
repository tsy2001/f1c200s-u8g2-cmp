#include <stdio.h>
#include <stdlib.h>
#include <stdbool.h>
#include <stdint.h>
#include <limits.h>
#include <string.h>
#include <strings.h>
#include <dirent.h>
#include <fcntl.h>
#include <sys/types.h>
#include <sys/mount.h>
#include <sys/stat.h>
#include <sys/ioctl.h>
#include <errno.h>
#include <linux/cdrom.h>
#include <unistd.h>
#include <time.h>
#include <alsa/asoundlib.h>
#include <dr_flac.h>
#include <dr_wav.h>
#include <dr_mp3.h>
#include "dsd2pcm/dsd2pcm.h"
#include <app_cdplayer.h>

#define MOUNT_POINT "/mnt"

// 可解码的音频文件格式
typedef enum {
    AUDIO_FORMAT_UNKNOWN = 0,
    AUDIO_FORMAT_FLAC,
    AUDIO_FORMAT_WAV,
    AUDIO_FORMAT_MP3,
    AUDIO_FORMAT_DSD
} audio_format_t;

// 播放汉书返回值
#define AUDIO_OK      0
#define AUDIO_NEXT    1
#define AUDIO_PREV    2
#define AUDIO_EJECT   3
#define AUDIO_JUMP    4
#define AUDIO_ERROR   (-1)

// alsa缓冲区大小定义
#define ALSA_PERIOD_FRAMES 2048
#define ALSA_BUFFER_FRAMES 8192
#define DSD_BLOCK_FRAMES 8192

static inline void to_stereo_s16(const int16_t *in, uint64_t frames, unsigned int channels, int16_t *out)
{
    if (channels <= 1)
    {
        for (uint64_t i = 0; i < frames; i++)
        {
            int16_t s = in[i];
            out[i * 2] = s;
            out[i * 2 + 1] = s;
        }
        return;
    }

    for (uint64_t i = 0; i < frames; i++)
    {
        out[i * 2] = in[i * channels];
        out[i * 2 + 1] = in[i * channels + 1];
    }
}

static inline void to_stereo_s32(const int32_t *in, uint64_t frames, unsigned int channels, int32_t *out)
{
    if (channels <= 1)
    {
        for (uint64_t i = 0; i < frames; i++)
        {
            int32_t s = in[i];
            out[i * 2] = s;
            out[i * 2 + 1] = s;
        }
        return;
    }

    for (uint64_t i = 0; i < frames; i++)
    {
        out[i * 2] = in[i * channels];
        out[i * 2 + 1] = in[i * channels + 1];
    }
}

// 函数声明
int mount_dvd(const char *dev);
int mount_sdmmc(const char *dev);
void scan_and_play_audio(const char *path, APP_CDIO *pCDIO);
int play_audio(const char *file_path, APP_CDIO *pCDIO);
audio_format_t detect_audio_format(const char *file_path);
int play_flac(const char *file_path, APP_CDIO *pCDIO);
int play_wav(const char *file_path, APP_CDIO *pCDIO);
int play_mp3(const char *file_path, APP_CDIO *pCDIO);
int play_dsd(const char *file_path, APP_CDIO *pCDIO);
int reconfigure_alsa_rate(snd_pcm_t *pcm_handle, unsigned int sample_rate, uint8_t format);

typedef struct {
    uint32_t sample_rate;
    uint32_t channels;
    uint64_t sample_count;
    uint64_t data_offset;
    uint64_t data_size;
    uint32_t block_size;
    int lsbitfirst;
    int interleaved;
} dsd_info_t;

static int read_exact(FILE *f, void *buf, size_t len)
{
    return fread(buf, 1, len, f) == len ? 0 : -1;
}

static uint16_t read_u16be(const unsigned char *p)
{
    return (uint16_t)((p[0] << 8) | p[1]);
}

static uint32_t read_u32be(const unsigned char *p)
{
    return ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) | ((uint32_t)p[2] << 8) | (uint32_t)p[3];
}

static uint64_t read_u64be(const unsigned char *p)
{
    return ((uint64_t)p[0] << 56) | ((uint64_t)p[1] << 48) | ((uint64_t)p[2] << 40) | ((uint64_t)p[3] << 32) |
           ((uint64_t)p[4] << 24) | ((uint64_t)p[5] << 16) | ((uint64_t)p[6] << 8) | (uint64_t)p[7];
}

static uint32_t read_u32le(const unsigned char *p)
{
    return ((uint32_t)p[3] << 24) | ((uint32_t)p[2] << 16) | ((uint32_t)p[1] << 8) | (uint32_t)p[0];
}

static uint64_t read_u64le(const unsigned char *p)
{
    return ((uint64_t)p[7] << 56) | ((uint64_t)p[6] << 48) | ((uint64_t)p[5] << 40) | ((uint64_t)p[4] << 32) |
           ((uint64_t)p[3] << 24) | ((uint64_t)p[2] << 16) | ((uint64_t)p[1] << 8) | (uint64_t)p[0];
}

static int skip_fwd(FILE *f, uint64_t bytes)
{
    if (bytes == 0)
        return 0;
    if (bytes > (uint64_t)INT64_MAX)
        return -1;
    return fseeko(f, (off_t)bytes, SEEK_CUR);
}

static int parse_dsf(FILE *f, dsd_info_t *info)
{
    unsigned char hdr[28];
    if (read_exact(f, hdr, sizeof(hdr)) < 0)
        return -1;
    if (memcmp(hdr, "DSD ", 4) != 0)
        return -1;

    int got_fmt = 0;
    int got_data = 0;
    while (!got_data)
    {
        unsigned char id[4];
        unsigned char size_buf[8];
        if (read_exact(f, id, sizeof(id)) < 0)
            break;
        if (read_exact(f, size_buf, sizeof(size_buf)) < 0)
            break;
        uint64_t chunk_size = read_u64le(size_buf);
        if (chunk_size == 0)
            return -1;

        uint64_t data_size = chunk_size;
        if (chunk_size == 52)
            data_size = 40;
        else if (chunk_size > 12)
            data_size = chunk_size - 12;

        if (memcmp(id, "fmt ", 4) == 0)
        {
            unsigned char fmt_buf[40];
            if (data_size < sizeof(fmt_buf))
                return -1;
            if (read_exact(f, fmt_buf, sizeof(fmt_buf)) < 0)
                return -1;
            if (data_size > sizeof(fmt_buf))
            {
                if (skip_fwd(f, data_size - sizeof(fmt_buf)) < 0)
                    return -1;
            }
            info->channels = read_u32le(fmt_buf + 12);
            info->sample_rate = read_u32le(fmt_buf + 16);
            info->sample_count = read_u64le(fmt_buf + 24);
            info->block_size = read_u32le(fmt_buf + 32);
            info->lsbitfirst = 1;
            info->interleaved = 0;
            got_fmt = 1;
        }
        else if (memcmp(id, "data", 4) == 0)
        {
            unsigned char ds_buf[8];
            if (read_exact(f, ds_buf, sizeof(ds_buf)) < 0)
                return -1;
            info->data_size = read_u64le(ds_buf);
            if (info->data_size == 0 && chunk_size > 20)
                info->data_size = chunk_size - 20;
            info->data_offset = (uint64_t)ftello(f);
            info->lsbitfirst = 1;
            info->interleaved = 0;
            got_data = 1;
            if (got_fmt)
                return 0;
        }
        else
        {
            if (skip_fwd(f, data_size) < 0)
                return -1;
        }
    }

    return (got_fmt && got_data) ? 0 : -1;
}

static int parse_dff(FILE *f, dsd_info_t *info)
{
    unsigned char hdr[12];
    if (read_exact(f, hdr, sizeof(hdr)) < 0)
        return -1;
    if (memcmp(hdr, "FRM8", 4) != 0)
        return -1;

    uint64_t form_size = read_u64be(hdr + 4);
    unsigned char form_type[4];
    if (read_exact(f, form_type, sizeof(form_type)) < 0)
        return -1;
    if (memcmp(form_type, "DSD ", 4) != 0)
        return -1;

    int got_prop = 0;
    int got_data = 0;
    off_t form_end = ftello(f);
    if (form_size < 4)
        return -1;
    if (form_size > (uint64_t)INT64_MAX)
        return -1;
    form_end += (off_t)(form_size - 4);

    while (ftello(f) < form_end)
    {
        unsigned char id[4];
        unsigned char size_buf[8];
        if (read_exact(f, id, sizeof(id)) < 0)
            break;
        if (read_exact(f, size_buf, sizeof(size_buf)) < 0)
            break;
        uint64_t chunk_size = read_u64be(size_buf);
        if (chunk_size == 0)
            return -1;
        off_t data_start = ftello(f);

        if (memcmp(id, "PROP", 4) == 0)
        {
            unsigned char prop_type[4];
            if (read_exact(f, prop_type, sizeof(prop_type)) < 0)
                return -1;
            uint64_t prop_remaining = chunk_size >= 4 ? chunk_size - 4 : 0;
            if (memcmp(prop_type, "SND ", 4) != 0)
            {
                if (skip_fwd(f, prop_remaining) < 0)
                    return -1;
            }
            else
            {
                off_t prop_end = ftello(f) + (off_t)prop_remaining;
                while (ftello(f) + 12 <= prop_end)
                {
                    unsigned char pid[4];
                    unsigned char psz_buf[8];
                    if (read_exact(f, pid, sizeof(pid)) < 0)
                        return -1;
                    if (read_exact(f, psz_buf, sizeof(psz_buf)) < 0)
                        return -1;
                    uint64_t psz = read_u64be(psz_buf);
                    off_t pstart = ftello(f);

                    if (memcmp(pid, "FS  ", 4) == 0 && psz >= 4)
                    {
                        unsigned char fs_buf[4];
                        if (read_exact(f, fs_buf, sizeof(fs_buf)) < 0)
                            return -1;
                        info->sample_rate = read_u32be(fs_buf);
                    }
                    else if (memcmp(pid, "CHNL", 4) == 0 && psz >= 2)
                    {
                        unsigned char ch_buf[2];
                        if (read_exact(f, ch_buf, sizeof(ch_buf)) < 0)
                            return -1;
                        info->channels = read_u16be(ch_buf);
                    }
                    else if (memcmp(pid, "CMPR", 4) == 0 && psz >= 4)
                    {
                        unsigned char cmpr_buf[4];
                        if (read_exact(f, cmpr_buf, sizeof(cmpr_buf)) < 0)
                            return -1;
                        if (memcmp(cmpr_buf, "DSD ", 4) != 0)
                            return -1;
                    }

                    uint64_t consumed = (uint64_t)(ftello(f) - pstart);
                    if (consumed > psz)
                        return -1;
                    if (skip_fwd(f, psz - consumed) < 0)
                        return -1;
                    if (psz & 1)
                        if (skip_fwd(f, 1) < 0)
                            return -1;
                }
            }
            got_prop = 1;
        }
        else if (memcmp(id, "DSD ", 4) == 0)
        {
            info->data_offset = (uint64_t)data_start;
            info->data_size = chunk_size;
            info->lsbitfirst = 0;
            info->interleaved = 1;
            got_data = 1;
        }
        else if (memcmp(id, "DST ", 4) == 0)
        {
            return -1;
        }

        if (chunk_size > (uint64_t)INT64_MAX)
            return -1;
        if (fseeko(f, data_start + (off_t)chunk_size, SEEK_SET) < 0)
            return -1;
        if (chunk_size & 1)
            if (skip_fwd(f, 1) < 0)
                return -1;
    }

    if (!got_prop || !got_data || info->channels == 0 || info->sample_rate == 0)
        return -1;

    return 0;
}

static inline int16_t dsd_float_to_s16(float v)
{
    float scaled = v * 32767.0f;
    int32_t s = (int32_t)(scaled >= 0.0f ? (scaled + 0.5f) : (scaled - 0.5f));
    if (s > 32767)
        s = 32767;
    else if (s < -32768)
        s = -32768;
    return (int16_t)s;
}

int cdrom_set_speed(const char *dev_path, int speed_x)
{
    if (!dev_path) return -EINVAL;
    if (speed_x < 0) return -EINVAL;

    // O_NONBLOCK: avoid blocking on some drives when no media / during spin-up
    int fd = open(dev_path, O_RDONLY | O_NONBLOCK);
    if (fd < 0) return -errno;

    // CDROM_SELECT_SPEED: speed in X; 0 means "auto"
    int rc = ioctl(fd, CDROM_SELECT_SPEED, speed_x);
    int saved_errno = errno;

    close(fd);

    if (rc < 0) return -saved_errno;
    return 0;
}

int play_dvd(const char *cd_dev, APP_CDIO *pCDIO)
{
    cdrom_set_speed(cd_dev, 4);
    
    if (mount_dvd(cd_dev) != 0)
    {
        fprintf(stderr, "Failed to mount DVD from %s.\n", cd_dev);
        return 1;
    }

    printf("Scanning and playing audio files from %s\n", MOUNT_POINT);
    scan_and_play_audio(MOUNT_POINT, pCDIO);

    umount(MOUNT_POINT);
    return 0;
}

int play_sdmmc(const char *dev, APP_CDIO *pCDIO)
{
    if (mount_sdmmc(dev) != 0)
    {
        fprintf(stderr, "Failed to mount SDMMC from %s.\n", dev);
        return 1;
    }

    printf("Scanning and playing audio files from %s\n", MOUNT_POINT);
    scan_and_play_audio(MOUNT_POINT, pCDIO);

    umount(MOUNT_POINT);
    return 0;
}

// 配置ALSA采样率
int reconfigure_alsa_rate(snd_pcm_t *pcm_handle, unsigned int sample_rate, uint8_t format)
{
    int err;

    printf("=== Starting ALSA reconfiguration to %u Hz ===\n", sample_rate);

    snd_pcm_format_t fmt;
    switch (format) {
        case 8:
            fmt = SND_PCM_FORMAT_S8;
            break;
        case 16:
            fmt = SND_PCM_FORMAT_S16_LE;
            break;
        case 24:
            fmt = SND_PCM_FORMAT_S24_LE;
            break;
        case 32:
            fmt = SND_PCM_FORMAT_S32_LE;
            break;
        default:
            fmt = SND_PCM_FORMAT_S16_LE;
            break;
    }

    snd_pcm_state_t state = snd_pcm_state(pcm_handle);
    printf("Current PCM state: %d\n", state);
    
    // 放完缓冲区内的音频
    if (state == SND_PCM_STATE_RUNNING || state == SND_PCM_STATE_XRUN || 
        state == SND_PCM_STATE_PREPARED || state == SND_PCM_STATE_PAUSED) {
        printf("Dropping PCM...\n");
        err = snd_pcm_drain(pcm_handle);
        if (err < 0) {
            fprintf(stderr, "snd_pcm_drain failed: %s\n", snd_strerror(err));
        }
    }
    
    printf("Resetting PCM...\n");
    err = snd_pcm_reset(pcm_handle);
    if (err < 0) {
        fprintf(stderr, "snd_pcm_reset failed: %s\n", snd_strerror(err));
        // 这个出错无所谓
    }

    printf("Recovering PCM...\n");
    snd_pcm_recover(pcm_handle, -ESTRPIPE, 1);

    /*snd_pcm_hw_params_t *params = malloc(snd_pcm_hw_params_sizeof());
    if (!params) {
        fprintf(stderr, "malloc failed for ALSA params\n");
        return -1;
    }

    printf("Setting up hardware parameters...\n");
    err = snd_pcm_hw_params_any(pcm_handle, params);
    if (err < 0) {
        fprintf(stderr, "snd_pcm_hw_params_any failed: %s\n", snd_strerror(err));
        free(params);
        return -1;
    }

    err = snd_pcm_hw_params_set_format(pcm_handle, params, SND_PCM_FORMAT_S16_LE);
    if (err < 0) {
        fprintf(stderr, "snd_pcm_hw_params_set_format failed: %s\n", snd_strerror(err));
        free(params);
        return -1;
    }

    err = snd_pcm_hw_params_set_access(pcm_handle, params, SND_PCM_ACCESS_RW_INTERLEAVED);
    if (err < 0) {
        fprintf(stderr, "snd_pcm_hw_params_set_access failed: %s\n", snd_strerror(err));
        free(params);
        return -1;
    }

    err = snd_pcm_hw_params_set_channels(pcm_handle, params, 2);
    if (err < 0) {
        fprintf(stderr, "snd_pcm_hw_params_set_channels failed: %s\n", snd_strerror(err));
        free(params);
        return -1;
    }

    err = snd_pcm_hw_params_set_rate(pcm_handle, params, sample_rate, 0);
    if (err < 0) {
        fprintf(stderr, "snd_pcm_hw_params_set_rate(%u) failed: %s\n", sample_rate, snd_strerror(err));
        free(params);
        return -1;
    }

    snd_pcm_uframes_t period = ALSA_PERIOD_FRAMES;
    snd_pcm_uframes_t buffer = ALSA_BUFFER_FRAMES;
    err = snd_pcm_hw_params_set_period_size_near(pcm_handle, params, &period, 0);
    if (err < 0)
    {
        // 设置失败则强制设置
        snd_pcm_hw_params_set_period_size(pcm_handle, params, ALSA_PERIOD_FRAMES, 0);
    }

    err = snd_pcm_hw_params_set_buffer_size_near(pcm_handle, params, &buffer);
    if (err < 0)
    {
        snd_pcm_hw_params_set_buffer_size(pcm_handle, params, ALSA_BUFFER_FRAMES);
    }

    printf("Applying hardware parameters...\n");
    err = snd_pcm_hw_params(pcm_handle, params);
    if (err < 0) {
        fprintf(stderr, "snd_pcm_hw_params failed: %s\n", snd_strerror(err));
        free(params);
        return -1;
    }*/

    // 这是另一种配置方式，如果codec不支持采样率可以让alsalib自己转换采样率
    printf("Applying Alsa parameters...\n");
    err = snd_pcm_set_params(pcm_handle,
                       fmt,
                       SND_PCM_ACCESS_RW_INTERLEAVED,
                       2,
                       sample_rate, 
                       1,
                       400000);
    if (err < 0) {
        fprintf(stderr, "snd_pcm_set_params failed: %s\n", snd_strerror(err));
        return -1;
    }

    printf("Preparing PCM...\n");
    err = snd_pcm_prepare(pcm_handle);
    if (err < 0) {
        fprintf(stderr, "snd_pcm_prepare failed: %s\n", snd_strerror(err));
        // free(params);
        return -1;
    }

    printf("=== ALSA reconfiguration to %u Hz complete ===\n", sample_rate);
    // free(params);
    return 0;
}

// 写音频数据到ALSA
int write_audio_to_alsa(snd_pcm_t *pcm_handle, int16_t *data, uint64_t frames,
                        unsigned int channels __attribute__((unused)), APP_CDIO *pCDIO)
{
    if (frames == 0)
        return 0;

    int frames_written = snd_pcm_writei(pcm_handle, data, (int)frames);
    if (frames_written < 0) {
        int err = snd_pcm_recover(pcm_handle, frames_written, 0);
        if (err < 0) {
            fprintf(stderr, "ALSA recover failed: %s\n", snd_strerror(err));
            snd_pcm_drain(pcm_handle);  // drain or drop
            snd_pcm_prepare(pcm_handle);
        }
        return frames_written;
    }
    
    if (frames_written > 0) {
        pthread_mutex_lock(&pCDIO->lock);
        pCDIO->now_lsn += frames_written; // 更新UI播放位置
        pthread_mutex_unlock(&pCDIO->lock);
    }

    return frames_written;
}

int write_audio_to_alsa_s32(snd_pcm_t *pcm_handle, int32_t *data, uint64_t frames,
                            unsigned int channels __attribute__((unused)), APP_CDIO *pCDIO)
{
    if (frames == 0)
        return 0;

    int frames_written = snd_pcm_writei(pcm_handle, data, (int)frames);
    if (frames_written < 0) {
        int err = snd_pcm_recover(pcm_handle, frames_written, 0);
        if (err < 0) {
            fprintf(stderr, "ALSA recover failed: %s\n", snd_strerror(err));
            snd_pcm_drain(pcm_handle);
            snd_pcm_prepare(pcm_handle);
        }
        return frames_written;
    }

    if (frames_written > 0) {
        pthread_mutex_lock(&pCDIO->lock);
        pCDIO->now_lsn += frames_written;
        pthread_mutex_unlock(&pCDIO->lock);
    }

    return frames_written;
}

int mount_dvd(const char *dev)
{
    if (access(MOUNT_POINT, F_OK) != 0)
    {
        if (mkdir(MOUNT_POINT, 0755) != 0) // 如果没有挂载点就创建
        {
            perror("Failed to create mount point");
            return -1;
        }
    }

    const char *fstypes[] = {"udf", "iso9660"};
    for (size_t i = 0; i < sizeof(fstypes) / sizeof(fstypes[0]); i++)
    {
        if (mount(dev, MOUNT_POINT, fstypes[i], MS_RDONLY, NULL) == 0)
            return 0;
        printf("Failed to mount DVD by: %s\n", fstypes[i]);
    }

    return -1;
}

int mount_sdmmc(const char *dev)
{
    if (access(MOUNT_POINT, F_OK) != 0)
    {
        if (mkdir(MOUNT_POINT, 0755) != 0) // 如果没有挂载点就创建
        {
            perror("Failed to create mount point");
            return -1;
        }
    }

    if (mount(dev, MOUNT_POINT, "vfat", MS_RDONLY, NULL) == 0)
        return 0;

    printf("Failed to mount SDMMC device\n");
    return -1;
}

// 获取音频文件的数量，并返回文件路径数组
int scan_audio_files(const char *path, char ***audio_files)
{
    DIR *dir;
    struct dirent *entry;
    int count = 0;

    if ((dir = opendir(path)) == NULL)
    {
        perror("Failed to open directory");
        return -1;
    }

    while ((entry = readdir(dir)) != NULL)
    {   // 统计可识别的音频文件数量
        audio_format_t fmt = detect_audio_format(entry->d_name);
        if (fmt != AUDIO_FORMAT_UNKNOWN)
            count++;
    }

    if (count == 0)
    {
        printf("No audio files found.\n");
        closedir(dir);
        return 0;
    }

    // 分配内存，放文件路径
    *audio_files = (char **)malloc(count * sizeof(char *));
    if (*audio_files == NULL)
    {
        perror("Memory allocation failed");
        closedir(dir);
        return -1;
    }

    rewinddir(dir);
    int index = 0;
    while ((entry = readdir(dir)) != NULL)
    {
        audio_format_t fmt = detect_audio_format(entry->d_name);
        if (fmt != AUDIO_FORMAT_UNKNOWN)
        {
            // 拼接完整路径
            size_t full_len = strlen(path) + strlen(entry->d_name) + 2;
            char *full_path = (char *)malloc(full_len);
            if (!full_path)
            {
                perror("Memory allocation failed for audio path");
                for (int i = 0; i < index; i++)
                    free((*audio_files)[i]);
                free(*audio_files);
                *audio_files = NULL;
                closedir(dir);
                return -1;
            }
            snprintf(full_path, full_len, "%s/%s", path, entry->d_name);
            (*audio_files)[index] = full_path;
            index++;
        }
    }

    closedir(dir);
    return count;
}

// 根据文件扩展名检测音频格式
audio_format_t detect_audio_format(const char *file_path)
{
    // macOS拷贝的文件会产生._垃圾文件，忽略它们
    const char *name = strrchr(file_path, '/');
    name = name ? name + 1 : file_path;
    if (name[0] == '.' && name[1] == '_')
        return AUDIO_FORMAT_UNKNOWN;

    const char *ext = strrchr(file_path, '.');
    if (!ext)
        return AUDIO_FORMAT_UNKNOWN;

    if (strcasecmp(ext, ".flac") == 0)
        return AUDIO_FORMAT_FLAC;
    if (strcasecmp(ext, ".wav") == 0)
        return AUDIO_FORMAT_WAV;
    if (strcasecmp(ext, ".mp3") == 0)
        return AUDIO_FORMAT_MP3;
    if (strcasecmp(ext, ".dsf") == 0 || strcasecmp(ext, ".dff") == 0)
        return AUDIO_FORMAT_DSD;

    return AUDIO_FORMAT_UNKNOWN;
}

// 从/dev/urandom读取随机数据
int read_urandom(void *buf, size_t len)
{
    int urandom_fd = open("/dev/urandom", O_RDONLY);
    if (urandom_fd < 0) return -1;

    size_t off = 0;
    while (off < len)
    {
        ssize_t n = read(urandom_fd, (char *)buf + off, len - off);
        if (n < 0)
        {
            if (errno == EINTR) continue;
            close(urandom_fd);
            return -1;
        }
        if (n == 0)
        {
            close(urandom_fd);
            return -1;
        }
        off += (size_t)n;
    }

    close(urandom_fd);
    return 0;
}

// 初始化伪随机数生成器
void seed_prng_once(void)
{
    static int seeded = 0;
    if (seeded) return;

    // 构造一个不会重复的32位种子值：当前时间、进程ID、栈地址、/dev/urandom数据
    uint32_t seed = (uint32_t)time(NULL) ^ (uint32_t)getpid() ^ (uint32_t)(uintptr_t)&seed;
    uint32_t extra = 0;
    if (read_urandom(&extra, sizeof(extra)) == 0) seed ^= extra;
    srand(seed);
    seeded = 1;
}

// 生成一个32位随机数
uint32_t random_u32(void)
{
    uint32_t r = 0;
    if (read_urandom(&r, sizeof(r)) == 0)
        return r;

    seed_prng_once();
    r = (uint32_t)rand();
    r = (r << 16) ^ (uint32_t)rand();
    return r;
}

// 限幅
uint32_t random_bounded_u32(uint32_t bound)
{
    if (bound == 0)
        return 0;

    uint32_t limit = UINT32_MAX - (UINT32_MAX % bound);
    uint32_t r = 0;
    do
    {
        r = random_u32();
    } while (r >= limit);
    return r % bound;
}

// 生成一个不重复的随机索引数组
void generate_random_indices(int *indices, int count)
{
    // 填充数组
    for (int i = 0; i < count; i++)
    {
        indices[i] = i;
    }
    // 打乱数组
    for (int i = count - 1; i > 0; i--)
    {
        uint32_t j = random_bounded_u32((uint32_t)i + 1);
        int temp = indices[i];
        indices[i] = indices[j];
        indices[j] = temp;
    }
}

void scan_and_play_audio(const char *path, APP_CDIO *pCDIO)
{
    char **audio_files = NULL;
    int count = scan_audio_files(path, &audio_files);
    if (count <= 0)
        return;

    // 同步ui
    pthread_mutex_lock(&pCDIO->lock);
    pCDIO->total_tracks = count;
    pCDIO->cdda_ready_flag = 1;
    pthread_mutex_unlock(&pCDIO->lock);

    // 生成随即播放顺序
    int *random_indices = (int *)malloc(count * sizeof(int));
    if (random_indices == NULL)
    {
        perror("Memory allocation for random indices failed");
        goto cleanup_files;
    }
    generate_random_indices(random_indices, count);

    int pos = 0;
    while (1)
    {
        if (pos < 0)
            pos = 0;
        if (pos >= count)
            pos = 0;

        int idx = random_indices[pos];

        // 更新ui
        pthread_mutex_lock(&pCDIO->lock);
        pCDIO->now_tracks = pos + 2;
        pthread_mutex_unlock(&pCDIO->lock);

        int rc = play_audio(audio_files[idx], pCDIO); // 单个文件播放循环
        // 播放完了一首文件，按返回值处理下一步
        if (rc == AUDIO_ERROR)
            break;
        else if (rc == AUDIO_EJECT)
            break;
        else if (rc == AUDIO_NEXT)
        {
            pthread_mutex_lock(&pCDIO->lock);
            pCDIO->next = 0;
            pthread_mutex_unlock(&pCDIO->lock);
            pos++;
            continue;
        }
        else if (rc == AUDIO_PREV)
        {
            pthread_mutex_lock(&pCDIO->lock);
            pCDIO->prev = 0;
            pthread_mutex_unlock(&pCDIO->lock);
            pos--;
            continue;
        } 
        else if (rc == AUDIO_JUMP)
        {
            pthread_mutex_lock(&pCDIO->lock);
            unsigned int manual_track = pCDIO->manual_track;
            pCDIO->manual_track = 0;
            pthread_mutex_unlock(&pCDIO->lock);
            if (manual_track >= 1 && manual_track <= (unsigned int)count)
            {
                pos = manual_track - 1;
            }
        }
        else
        {
            pos++;
            continue;
        }
    }

cleanup_files:
    for (int i = 0; i < count; i++)
        free(audio_files[i]);
    free(audio_files);
    if (random_indices)
        free(random_indices);
}

// wav格式解码
int play_wav(const char *file_path, APP_CDIO *pCDIO)
{
    drwav wav;
    if (!drwav_init_file(&wav, file_path, NULL))
    {
        fprintf(stderr, "Failed to open WAV file: %s\n", file_path);
        return AUDIO_ERROR;
    }

    printf("WAV: channels=%u, sample_rate=%u, bit_per_sample=%u, total_frames=%llu\n",
           wav.channels, wav.sampleRate, wav.bitsPerSample, wav.totalPCMFrameCount);

    // 配置ALSA采样率
    uint8_t out_bits = (wav.bitsPerSample > 16) ? 32 : 16;
    if (reconfigure_alsa_rate(pCDIO->pcm_handle, wav.sampleRate, out_bits) < 0) {
        fprintf(stderr, "Failed to reconfigure ALSA for WAV\n");
        drwav_uninit(&wav);
        return AUDIO_ERROR;
    }

    // 更新ui
    pthread_mutex_lock(&pCDIO->lock);
    pCDIO->total_lsn = (lsn_t)wav.totalPCMFrameCount;
    pCDIO->now_lsn = 0;
    pthread_mutex_unlock(&pCDIO->lock);

    // 分配缓冲区
    size_t buffer_frames = 16384;
    bool use_s32 = wav.bitsPerSample > 16;
    void *sample_buf = NULL;
    void *stereo_buf = NULL;
    if (use_s32)
        sample_buf = malloc(buffer_frames * wav.channels * sizeof(int32_t));
    else
        sample_buf = malloc(buffer_frames * wav.channels * sizeof(int16_t));
    if (!sample_buf)
    {
        perror("malloc failed for WAV samples");
        drwav_uninit(&wav);
        return AUDIO_ERROR;
    }
    if (wav.channels != 2)
    {
        if (use_s32)
            stereo_buf = malloc(buffer_frames * 2 * sizeof(int32_t));
        else
            stereo_buf = malloc(buffer_frames * 2 * sizeof(int16_t));
        if (!stereo_buf)
        {
            perror("malloc failed for WAV stereo buffer");
            free(sample_buf);
            drwav_uninit(&wav);
            return AUDIO_ERROR;
        }
    }

    int16_t *pSampleData = (int16_t *)sample_buf;
    int32_t *pSampleData32 = (int32_t *)sample_buf;

    uint64_t frames_read = 0;

    // 解码循环
    while (frames_read < wav.totalPCMFrameCount)
    {
        // 暂存原子量
        pthread_mutex_lock(&pCDIO->lock);
        uint8_t next = pCDIO->next;
        uint8_t prev = pCDIO->prev;
        uint8_t ejct = pCDIO->ejct;
        uint8_t stop_flag = pCDIO->stop;
        uint16_t manual_track = pCDIO->manual_track;
        
        pCDIO->next = 0;
        pCDIO->prev = 0;
        pthread_mutex_unlock(&pCDIO->lock);

        if (next || prev || ejct || manual_track != 0)
        {
            if(manual_track > pCDIO->total_tracks || manual_track + 1 == pCDIO->now_tracks)
            {
                pCDIO->manual_track = 0;
                continue;
            }
            // 播放完缓冲区内的数据
            int st = snd_pcm_state(pCDIO->pcm_handle);
            if (st == SND_PCM_STATE_RUNNING || st == SND_PCM_STATE_XRUN ||
                st == SND_PCM_STATE_SUSPENDED || st == SND_PCM_STATE_DISCONNECTED)
            {
                printf("Draining playback\n");
                snd_pcm_drain(pCDIO->pcm_handle);
                snd_pcm_prepare(pCDIO->pcm_handle);
            }
            
            free(stereo_buf);
            free(sample_buf);
            drwav_uninit(&wav);
            
            if (next)
                return AUDIO_NEXT;
            if (prev)
                return AUDIO_PREV;
            if (ejct)
                return AUDIO_EJECT;
            if (manual_track != 0)
                return AUDIO_JUMP;
        }

        // 暂停处理
        if (stop_flag) {
            app_cdio_set_mute(true);
            sleep(1);
            continue;
        } else {
            app_cdio_set_mute(false);
        }

        // dr_lib解码
        uint64_t samples_to_read = (wav.totalPCMFrameCount - frames_read > buffer_frames) ? 
                                   buffer_frames : (wav.totalPCMFrameCount - frames_read);
        uint64_t frames = 0;
        if (use_s32)
        {
            frames = drwav_read_pcm_frames_s32(&wav, samples_to_read, pSampleData32);
        }
        else
        {
            frames = drwav_read_pcm_frames_s16(&wav, samples_to_read, pSampleData);
        }

        if (frames == 0)
            break;

        // 软件处理音量
        float vol_snapshot;
        pthread_mutex_lock(&pCDIO->lock);
        vol_snapshot = pCDIO->volume;
        pthread_mutex_unlock(&pCDIO->lock);

        if (use_s32)
        {
            for (uint64_t i = 0; i < frames * wav.channels; i++)
            {
                int64_t scaled = (int64_t)(pSampleData32[i] * vol_snapshot);
                if (scaled > INT32_MAX)
                    scaled = INT32_MAX;
                else if (scaled < INT32_MIN)
                    scaled = INT32_MIN;
                pSampleData32[i] = (int32_t)scaled;
            }
        }
        else
        {
            for (uint64_t i = 0; i < frames * wav.channels; i++)
            {
                int32_t scaled = (int32_t)(pSampleData[i] * vol_snapshot);
                pSampleData[i] = (int16_t)(scaled > INT16_MAX ? INT16_MAX : (scaled < INT16_MIN ? INT16_MIN : scaled));
            }
        }

        // 写入ALSA
        int frames_written = 0;
        if (use_s32)
        {
            int32_t *out_buf = pSampleData32;
            if (wav.channels != 2)
            {
                to_stereo_s32(pSampleData32, frames, wav.channels, (int32_t *)stereo_buf);
                out_buf = (int32_t *)stereo_buf;
            }
            frames_written = write_audio_to_alsa_s32(pCDIO->pcm_handle, out_buf, frames, 2, pCDIO);
        }
        else
        {
            int16_t *out_buf = pSampleData;
            if (wav.channels != 2)
            {
                to_stereo_s16(pSampleData, frames, wav.channels, (int16_t *)stereo_buf);
                out_buf = (int16_t *)stereo_buf;
            }
            frames_written = write_audio_to_alsa(pCDIO->pcm_handle, out_buf, frames, 2, pCDIO);
        }
        if (frames_written < 0) {
            fprintf(stderr, "ALSA write error: %d\n", frames_written);
            // todo：出错了也没办法
        } else {
            frames_read += frames;
        }
    }

    free(stereo_buf);
    free(sample_buf);
    drwav_uninit(&wav);
    return AUDIO_OK;
}

int play_flac(const char *file_path, APP_CDIO *pCDIO)
{
    drflac *pFlac = drflac_open_file(file_path, NULL);
    if (!pFlac)
    {
        fprintf(stderr, "Failed to open FLAC file: %s\n", file_path);
        return AUDIO_ERROR;
    }

    printf("FLAC: channels=%u, sample_rate=%u, bit_per_sample=%u, total_frames=%llu\n",
           pFlac->channels, pFlac->sampleRate, pFlac->bitsPerSample, pFlac->totalPCMFrameCount);

    uint8_t out_bits = (pFlac->bitsPerSample > 16) ? 32 : 16;
    if (reconfigure_alsa_rate(pCDIO->pcm_handle, pFlac->sampleRate, out_bits) < 0) {
        fprintf(stderr, "Failed to reconfigure ALSA for FLAC\n");
        drflac_close(pFlac);
        return AUDIO_ERROR;
    }

    pthread_mutex_lock(&pCDIO->lock);
    pCDIO->total_lsn = (lsn_t)pFlac->totalPCMFrameCount;
    pCDIO->now_lsn = 0;
    pthread_mutex_unlock(&pCDIO->lock);

    size_t buffer_frames = 16384;
    bool use_s32 = pFlac->bitsPerSample > 16;
    void *sample_buf = NULL;
    void *stereo_buf = NULL;
    if (use_s32)
        sample_buf = malloc(pFlac->channels * buffer_frames * sizeof(drflac_int32));
    else
        sample_buf = malloc(pFlac->channels * buffer_frames * sizeof(drflac_int16));
    if (!sample_buf)
    {
        perror("malloc failed for FLAC samples");
        drflac_close(pFlac);
        return AUDIO_ERROR;
    }
    if (pFlac->channels != 2)
    {
        if (use_s32)
            stereo_buf = malloc(buffer_frames * 2 * sizeof(drflac_int32));
        else
            stereo_buf = malloc(buffer_frames * 2 * sizeof(drflac_int16));
        if (!stereo_buf)
        {
            perror("malloc failed for FLAC stereo buffer");
            free(sample_buf);
            drflac_close(pFlac);
            return AUDIO_ERROR;
        }
    }

    drflac_int16 *pSampleData = (drflac_int16 *)sample_buf;
    drflac_int32 *pSampleData32 = (drflac_int32 *)sample_buf;

    uint64_t frames_read = 0;
    int last_rc = AUDIO_OK;
    
    while (frames_read < pFlac->totalPCMFrameCount)
    {
        pthread_mutex_lock(&pCDIO->lock);
        uint8_t next = pCDIO->next;
        uint8_t prev = pCDIO->prev;
        uint8_t ejct = pCDIO->ejct;
        uint8_t stop_flag = pCDIO->stop;
        uint16_t manual_track = pCDIO->manual_track;

        pCDIO->next = 0;
        pCDIO->prev = 0;
        pthread_mutex_unlock(&pCDIO->lock);

        if (next || prev || ejct || manual_track != 0) {
            if(manual_track > pCDIO->total_tracks || manual_track + 1 == pCDIO->now_tracks)
            {
                pCDIO->manual_track = 0;
                continue;
            }
            last_rc = next ? AUDIO_NEXT : (prev ? AUDIO_PREV : (ejct ? AUDIO_EJECT : AUDIO_JUMP));
            break;
        }

        if (stop_flag) {
            app_cdio_set_mute(true);
            sleep(1);
            continue;
        } else {
            app_cdio_set_mute(false);
        }

        uint64_t samples_to_read = (pFlac->totalPCMFrameCount - frames_read > buffer_frames) ? 
                                   buffer_frames : (pFlac->totalPCMFrameCount - frames_read);
        uint64_t frames = 0;
        if (use_s32)
        {
            frames = drflac_read_pcm_frames_s32(pFlac, samples_to_read, pSampleData32);
        }
        else
        {
            frames = drflac_read_pcm_frames_s16(pFlac, samples_to_read, pSampleData);
        }

        if (frames == 0)
            break;

        float vol_snapshot;
        pthread_mutex_lock(&pCDIO->lock);
        vol_snapshot = pCDIO->volume;
        pthread_mutex_unlock(&pCDIO->lock);

        if (use_s32)
        {
            for (uint64_t i = 0; i < frames * pFlac->channels; i++)
            {
                int64_t scaled = (int64_t)(pSampleData32[i] * vol_snapshot);
                if (scaled > INT32_MAX)
                    scaled = INT32_MAX;
                else if (scaled < INT32_MIN)
                    scaled = INT32_MIN;
                pSampleData32[i] = (int32_t)scaled;
            }
        }
        else
        {
            for (uint64_t i = 0; i < frames * pFlac->channels; i++)
            {
                int32_t scaled = (int32_t)(pSampleData[i] * vol_snapshot);
                pSampleData[i] = (int16_t)(scaled > INT16_MAX ? INT16_MAX : (scaled < INT16_MIN ? INT16_MIN : scaled));
            }
        }

        int frames_written = 0;
        if (use_s32)
        {
            int32_t *out_buf = pSampleData32;
            if (pFlac->channels != 2)
            {
                to_stereo_s32(pSampleData32, frames, pFlac->channels, (int32_t *)stereo_buf);
                out_buf = (int32_t *)stereo_buf;
            }
            frames_written = write_audio_to_alsa_s32(pCDIO->pcm_handle, out_buf, frames, 2, pCDIO);
        }
        else
        {
            int16_t *out_buf = pSampleData;
            if (pFlac->channels != 2)
            {
                to_stereo_s16(pSampleData, frames, pFlac->channels, (int16_t *)stereo_buf);
                out_buf = (int16_t *)stereo_buf;
            }
            frames_written = write_audio_to_alsa(pCDIO->pcm_handle, out_buf, frames, 2, pCDIO);
        }
        if (frames_written < 0) {
            fprintf(stderr, "ALSA write error: %d\n", frames_written);
            // todo：flac 和 mp3出错概率较高
        } else {
            frames_read += frames;
        }
    }

    free(stereo_buf);
    free(sample_buf);
    drflac_close(pFlac);
    
    int st = snd_pcm_state(pCDIO->pcm_handle);
    if (st == SND_PCM_STATE_RUNNING || st == SND_PCM_STATE_XRUN ||
        st == SND_PCM_STATE_SUSPENDED || st == SND_PCM_STATE_DISCONNECTED)
    {
        printf("Draining playback\n");
        snd_pcm_drain(pCDIO->pcm_handle);
        snd_pcm_prepare(pCDIO->pcm_handle);
    }
    
    return last_rc;
}

int play_mp3(const char *file_path, APP_CDIO *pCDIO)
{
    drmp3 mp3;
    if (!drmp3_init_file(&mp3, file_path, NULL))
    {
        fprintf(stderr, "Failed to open MP3 file: %s\n", file_path);
        return AUDIO_ERROR;
    }

    printf("MP3: channels=%u, sample_rate=%u\n", mp3.channels, mp3.sampleRate);

    if (reconfigure_alsa_rate(pCDIO->pcm_handle, mp3.sampleRate, 16) < 0) {
        fprintf(stderr, "Failed to reconfigure ALSA for MP3\n");
        drmp3_uninit(&mp3);
        return AUDIO_ERROR;
    }

    pthread_mutex_lock(&pCDIO->lock);
    pCDIO->total_lsn = (lsn_t)mp3.totalPCMFrameCount;
    pCDIO->now_lsn = 0;
    pthread_mutex_unlock(&pCDIO->lock);

    size_t buffer_frames = 8192;
    int16_t *stereo_buf = NULL;
    int16_t *pSampleData = (int16_t *)malloc(mp3.channels * buffer_frames * sizeof(int16_t));
    if (!pSampleData)
    {
        perror("malloc failed for MP3 samples");
        drmp3_uninit(&mp3);
        return AUDIO_ERROR;
    }
    if (mp3.channels != 2)
    {
        stereo_buf = (int16_t *)malloc(buffer_frames * 2 * sizeof(int16_t));
        if (!stereo_buf)
        {
            perror("malloc failed for MP3 stereo buffer");
            free(pSampleData);
            drmp3_uninit(&mp3);
            return AUDIO_ERROR;
        }
    }

    int last_rc = AUDIO_OK;
    uint64_t frames_read = 0;
    
    while (frames_read < mp3.totalPCMFrameCount)
    {
        pthread_mutex_lock(&pCDIO->lock);
        uint8_t next = pCDIO->next;
        uint8_t prev = pCDIO->prev;
        uint8_t ejct = pCDIO->ejct;
        uint16_t manual_track = pCDIO->manual_track;
        uint8_t stop_flag = pCDIO->stop;

        pCDIO->next = 0;
        pCDIO->prev = 0;
        pthread_mutex_unlock(&pCDIO->lock);

        if (next || prev || ejct || manual_track != 0) {
            if(manual_track > pCDIO->total_tracks || manual_track + 1 == pCDIO->now_tracks)
            {
                pCDIO->manual_track = 0;
                continue;
            }
            last_rc = next ? AUDIO_NEXT : (prev ? AUDIO_PREV : (ejct ? AUDIO_EJECT : AUDIO_JUMP));
            break;
        }

        if (stop_flag) {
            app_cdio_set_mute(true);
            sleep(1);
            continue;
        } else {
            app_cdio_set_mute(false);
        }

        uint64_t samples_to_read = (mp3.totalPCMFrameCount - frames_read > buffer_frames) ? 
                                   buffer_frames : (mp3.totalPCMFrameCount - frames_read);
        uint64_t frames = drmp3_read_pcm_frames_s16(&mp3, samples_to_read, pSampleData);

        if (frames == 0)
            break;

        float vol_snapshot;
        pthread_mutex_lock(&pCDIO->lock);
        vol_snapshot = pCDIO->volume;
        pthread_mutex_unlock(&pCDIO->lock);

        for (uint64_t i = 0; i < frames * mp3.channels; i++)
        {
            int32_t scaled = (int32_t)(pSampleData[i] * vol_snapshot);
            pSampleData[i] = (int16_t)(scaled > INT16_MAX ? INT16_MAX : (scaled < INT16_MIN ? INT16_MIN : scaled));
        }

        int16_t *out_buf = pSampleData;
        if (mp3.channels != 2)
        {
            to_stereo_s16(pSampleData, frames, mp3.channels, stereo_buf);
            out_buf = stereo_buf;
        }

        int frames_written = write_audio_to_alsa(pCDIO->pcm_handle, out_buf, frames, 2, pCDIO);
        if (frames_written < 0) {
            fprintf(stderr, "ALSA write error: %d\n", frames_written);
        } else {
            frames_read += frames;
        }
    }

    free(stereo_buf);
    free(pSampleData);
    drmp3_uninit(&mp3);
    
    int st = snd_pcm_state(pCDIO->pcm_handle);
    if (st == SND_PCM_STATE_RUNNING || st == SND_PCM_STATE_XRUN ||
        st == SND_PCM_STATE_SUSPENDED || st == SND_PCM_STATE_DISCONNECTED)
    {
        printf("Draining playback\n");
        snd_pcm_drain(pCDIO->pcm_handle);
        snd_pcm_prepare(pCDIO->pcm_handle);
    }
    
    return last_rc;
}

int play_dsd(const char *file_path, APP_CDIO *pCDIO)
{
    FILE *f = fopen(file_path, "rb");
    if (!f)
    {
        fprintf(stderr, "Failed to open DSD file: %s\n", file_path);
        return AUDIO_ERROR;
    }

    unsigned char magic[4];
    if (read_exact(f, magic, sizeof(magic)) < 0)
    {
        fclose(f);
        return AUDIO_ERROR;
    }
    fseeko(f, 0, SEEK_SET);

    dsd_info_t info;
    memset(&info, 0, sizeof(info));
    int parse_rc = -1;
    if (memcmp(magic, "DSD ", 4) == 0)
        parse_rc = parse_dsf(f, &info);
    else if (memcmp(magic, "FRM8", 4) == 0)
        parse_rc = parse_dff(f, &info);

    if (parse_rc < 0 || info.channels == 0 || info.sample_rate == 0 || info.data_size == 0)
    {
        fprintf(stderr, "Unsupported or invalid DSD file: %s\n", file_path);
        fclose(f);
        return AUDIO_ERROR;
    }
    if (!info.interleaved && info.block_size == 0)
    {
        fprintf(stderr, "DSF block size missing: %s\n", file_path);
        fclose(f);
        return AUDIO_ERROR;
    }

    if (info.sample_rate % 8 != 0)
    {
        fprintf(stderr, "Unsupported DSD sample rate: %u\n", info.sample_rate);
        fclose(f);
        return AUDIO_ERROR;
    }

    unsigned int pcm_rate = info.sample_rate / 8;
    printf("DSD: channels=%u, sample_rate=%u, pcm_rate=%u, data_size=%llu\n",
           info.channels, info.sample_rate, pcm_rate, (unsigned long long)info.data_size);

    if (reconfigure_alsa_rate(pCDIO->pcm_handle, pcm_rate, 16) < 0)
    {
        fprintf(stderr, "Failed to reconfigure ALSA for DSD\n");
        fclose(f);
        return AUDIO_ERROR;
    }

    uint64_t frames_from_size = info.data_size / info.channels;
    uint64_t frames_from_samples = info.sample_count ? (info.sample_count / 8) : 0;
    uint64_t total_frames = frames_from_size;
    if (frames_from_samples > 0 && frames_from_samples < total_frames)
        total_frames = frames_from_samples;
    if (!info.interleaved && info.block_size > 0)
        total_frames = (total_frames / info.block_size) * info.block_size;

    pthread_mutex_lock(&pCDIO->lock);
    pCDIO->total_lsn = (lsn_t)total_frames;
    pCDIO->now_lsn = 0;
    pthread_mutex_unlock(&pCDIO->lock);

    int decode_channels = info.channels >= 2 ? 2 : 1;
    dsd2pcm_ctx **ctx = (dsd2pcm_ctx **)calloc((size_t)decode_channels, sizeof(dsd2pcm_ctx *));
    if (!ctx)
    {
        fclose(f);
        return AUDIO_ERROR;
    }
    for (int i = 0; i < decode_channels; i++)
    {
        ctx[i] = dsd2pcm_init();
        if (!ctx[i])
        {
            for (int j = 0; j < i; j++)
                dsd2pcm_destroy(ctx[j]);
            free(ctx);
            fclose(f);
            return AUDIO_ERROR;
        }
        dsd2pcm_reset(ctx[i]);
    }

    size_t block_frames = info.block_size ? info.block_size : DSD_BLOCK_FRAMES;
    if (info.channels == 0 || block_frames > SIZE_MAX / info.channels)
    {
        for (int i = 0; i < decode_channels; i++)
            dsd2pcm_destroy(ctx[i]);
        free(ctx);
        fclose(f);
        return AUDIO_ERROR;
    }
    size_t block_bytes = block_frames * info.channels;
    unsigned char *dsd_buf = (unsigned char *)malloc(block_bytes);
    float *float_buf = (float *)malloc(block_frames * sizeof(float));
    int16_t *pcm_buf = (int16_t *)malloc(block_frames * 2 * sizeof(int16_t));
    if (!dsd_buf || !float_buf || !pcm_buf)
    {
        fprintf(stderr, "malloc failed for DSD buffers\n");
        free(dsd_buf);
        free(float_buf);
        free(pcm_buf);
        for (int i = 0; i < decode_channels; i++)
            dsd2pcm_destroy(ctx[i]);
        free(ctx);
        fclose(f);
        return AUDIO_ERROR;
    }

    if (fseeko(f, (off_t)info.data_offset, SEEK_SET) < 0)
    {
        fprintf(stderr, "seek failed for DSD data\n");
        free(dsd_buf);
        free(float_buf);
        free(pcm_buf);
        for (int i = 0; i < decode_channels; i++)
            dsd2pcm_destroy(ctx[i]);
        free(ctx);
        fclose(f);
        return AUDIO_ERROR;
    }

    int last_rc = AUDIO_OK;
    if (total_frames > 0 && info.channels > 0 && total_frames > UINT64_MAX / info.channels)
    {
        free(dsd_buf);
        free(float_buf);
        free(pcm_buf);
        for (int i = 0; i < decode_channels; i++)
            dsd2pcm_destroy(ctx[i]);
        free(ctx);
        fclose(f);
        return AUDIO_ERROR;
    }
    uint64_t bytes_remaining = total_frames * info.channels;

    while (bytes_remaining > 0)
    {
        pthread_mutex_lock(&pCDIO->lock);
        uint8_t next = pCDIO->next;
        uint8_t prev = pCDIO->prev;
        uint8_t ejct = pCDIO->ejct;
        uint16_t manual_track = pCDIO->manual_track;
        uint8_t stop_flag = pCDIO->stop;

        pCDIO->next = 0;
        pCDIO->prev = 0;
        pthread_mutex_unlock(&pCDIO->lock);

        if (next || prev || ejct || manual_track != 0)
        {
            if (manual_track > pCDIO->total_tracks || manual_track + 1 == pCDIO->now_tracks)
            {
                pCDIO->manual_track = 0;
                continue;
            }
            last_rc = next ? AUDIO_NEXT : (prev ? AUDIO_PREV : (ejct ? AUDIO_EJECT : AUDIO_JUMP));
            break;
        }

        if (stop_flag)
        {
            app_cdio_set_mute(true);
            sleep(1);
            continue;
        }
        else
        {
            app_cdio_set_mute(false);
        }

        size_t want = block_bytes;
        if (bytes_remaining < want)
            want = (size_t)bytes_remaining;

        size_t got = fread(dsd_buf, 1, want, f);
        if (got == 0)
            break;
        bytes_remaining -= got;

        size_t bytes_per_channel = got / info.channels;
        if (bytes_per_channel == 0)
            break;

        size_t frames = bytes_per_channel;
        if (frames > block_frames)
            frames = block_frames;

        float vol_snapshot;
        pthread_mutex_lock(&pCDIO->lock);
        vol_snapshot = pCDIO->volume;
        pthread_mutex_unlock(&pCDIO->lock);

        const unsigned char *src0 = info.interleaved ? (dsd_buf + 0) : (dsd_buf + 0 * bytes_per_channel);
        ptrdiff_t stride0 = info.interleaved ? (ptrdiff_t)info.channels : 1;
        dsd2pcm_translate(ctx[0], frames, src0, stride0, info.lsbitfirst, float_buf, 1);
        for (size_t s = 0; s < frames; s++)
        {
            int16_t smp = dsd_float_to_s16(float_buf[s] * vol_snapshot);
            pcm_buf[s * 2] = smp;
            if (decode_channels == 1)
                pcm_buf[s * 2 + 1] = smp;
        }

        if (decode_channels == 2)
        {
            const unsigned char *src1 = info.interleaved ? (dsd_buf + 1) : (dsd_buf + 1 * bytes_per_channel);
            ptrdiff_t stride1 = info.interleaved ? (ptrdiff_t)info.channels : 1;
            dsd2pcm_translate(ctx[1], frames, src1, stride1, info.lsbitfirst, float_buf, 1);
            for (size_t s = 0; s < frames; s++)
            {
                int16_t smp = dsd_float_to_s16(float_buf[s] * vol_snapshot);
                pcm_buf[s * 2 + 1] = smp;
            }
        }

        int frames_written = write_audio_to_alsa(pCDIO->pcm_handle, pcm_buf, frames, 2, pCDIO);
        if (frames_written < 0)
        {
            fprintf(stderr, "ALSA write error: %d\n", frames_written);
        }
    }

    free(dsd_buf);
    free(float_buf);
    free(pcm_buf);
    for (int i = 0; i < decode_channels; i++)
        dsd2pcm_destroy(ctx[i]);
    free(ctx);
    fclose(f);

    int st = snd_pcm_state(pCDIO->pcm_handle);
    if (st == SND_PCM_STATE_RUNNING || st == SND_PCM_STATE_XRUN ||
        st == SND_PCM_STATE_SUSPENDED || st == SND_PCM_STATE_DISCONNECTED)
    {
        printf("Draining playback\n");
        snd_pcm_drain(pCDIO->pcm_handle);
        snd_pcm_prepare(pCDIO->pcm_handle);
    }

    return last_rc;
}

int play_audio(const char *file_path, APP_CDIO *pCDIO)
{
    if (!file_path)
    {
        fprintf(stderr, "play_audio: file_path is NULL\n");
        return AUDIO_ERROR;
    }
    audio_format_t fmt = detect_audio_format(file_path);
    const char *name = strrchr(file_path, '/');
    name = name ? name + 1 : file_path;
    pthread_mutex_lock(&pCDIO->lock);
    strncpy(pCDIO->now_title, name, ONE_ALBUM_LENGTH - 1);
    pCDIO->now_title[ONE_ALBUM_LENGTH - 1] = '\0';
    pthread_mutex_unlock(&pCDIO->lock);

    printf("Playing: %s (format=%d)\n", file_path, fmt);

    switch (fmt)
    {
        case AUDIO_FORMAT_FLAC:
            return play_flac(file_path, pCDIO);
        case AUDIO_FORMAT_WAV:
            return play_wav(file_path, pCDIO);
        case AUDIO_FORMAT_MP3:
            return play_mp3(file_path, pCDIO);
        case AUDIO_FORMAT_DSD:
            return play_dsd(file_path, pCDIO);
        default:
            fprintf(stderr, "unsupported audio format: %s\n", file_path);
            return AUDIO_ERROR;
    }
}
