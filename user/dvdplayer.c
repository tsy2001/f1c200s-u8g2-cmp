#include <stdio.h>
#include <stdlib.h>
#include <stdbool.h>
#include <stdint.h>
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
#include <app_cdplayer.h>

#define MOUNT_POINT "/mnt/dvd"

// 可解码的音频文件格式
typedef enum {
    AUDIO_FORMAT_UNKNOWN = 0,
    AUDIO_FORMAT_FLAC,
    AUDIO_FORMAT_WAV,
    AUDIO_FORMAT_MP3
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
void scan_and_play_audio(const char *path, APP_CDIO *pCDIO);
int play_audio(const char *file_path, APP_CDIO *pCDIO);
audio_format_t detect_audio_format(const char *file_path);
int play_flac(const char *file_path, APP_CDIO *pCDIO);
int play_wav(const char *file_path, APP_CDIO *pCDIO);
int play_mp3(const char *file_path, APP_CDIO *pCDIO);
int reconfigure_alsa_rate(snd_pcm_t *pcm_handle, unsigned int sample_rate, uint8_t format);

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

int play_sdmmc(const char *mount_point, APP_CDIO *pCDIO)
{
    if (access(mount_point, R_OK | X_OK) != 0)
    {
        fprintf(stderr, "SDMMC mount point is not accessible: %s.\n", mount_point);
        return 1;
    }

    printf("Scanning and playing audio files from %s\n", mount_point);
    scan_and_play_audio(mount_point, pCDIO);
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
        default:
            fprintf(stderr, "unsupported audio format: %s\n", file_path);
            return AUDIO_ERROR;
    }
}
