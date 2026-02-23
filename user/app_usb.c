#ifndef _POSIX_C_SOURCE
#define _POSIX_C_SOURCE 200809L
#endif

#include <sys/stat.h>
#include <sys/ioctl.h>
#include <sys/wait.h>
#include <sys/mount.h>
#include <time.h>
#include <stdio.h>
#include <string.h>
#include <errno.h>
#include <signal.h>
#include <dirent.h>
#include <fcntl.h>
#include <unistd.h>
#include <termios.h>
#include <stdlib.h>
#include <pthread.h>
#include <stdbool.h>
#include <stdint.h>
#include <ctype.h>
#include <alsa/asoundlib.h>
#include "app_usb.h"

#define USB_BRIGHTNESS_PATH "/sys/class/leds/usb/brightness"
#define USB_MODE_PATH "/sys/devices/platform/soc/1c13000.usb/musb-hdrc.1.auto/mode"
#define USB_GSERIAL_TTY "/dev/ttyGS0"
#define USB_UDC_NAME "musb-hdrc.1.auto"
#define USB_CONFIGFS_PATH "/sys/kernel/config"
#define USB_GADGET_BASE "/sys/kernel/config/usb_gadget"
#define USB_GADGET_NAME "g1"
#define USB_GADGET_PATH USB_GADGET_BASE "/" USB_GADGET_NAME
#define USB_GADGET_LANG "0x409"
#define USB_GADGET_CFG "c.1"
#define USB_GADGET_UAC1_FUNC "uac1.usb0"
#define USB_ENABLE_ACM_CONSOLE 0
#define USB_AUDIO_RATE 48000
#define USB_AUDIO_CHANNELS 2
#define USB_AUDIO_FRAME 256
#define USB_MAX_CAPTURE_DEVS 8
#define USB_CODEC_PLAYBACK_DEV "hw:1,0"

static pthread_t usb_audio_thread;
static pthread_mutex_t usb_audio_lock = PTHREAD_MUTEX_INITIALIZER;
static int usb_audio_running = 0;
static int usb_audio_stop = 0;
static int usb_codec_mixer_primed = 0;
static bool strcasestr_simple(const char *haystack, const char *needle);

typedef struct
{
    int dev;
    char name[64];
} usb_capture_dev_t;

static int enum_item_score(const char *item_name)
{
    int score = 0;

    if (strcasestr_simple(item_name, "dac")) score += 100;
    if (strcasestr_simple(item_name, "lineout") || strcasestr_simple(item_name, "line out")) score += 60;
    if (strcasestr_simple(item_name, "speaker") || strcasestr_simple(item_name, "spk")) score += 50;
    if (strcasestr_simple(item_name, "headphone") || strcasestr_simple(item_name, "hp")) score += 40;
    if (strcasestr_simple(item_name, "playback") || strcasestr_simple(item_name, "output")) score += 20;
    if (strcasestr_simple(item_name, "mic") || strcasestr_simple(item_name, "input")) score -= 60;
    if (strcasestr_simple(item_name, "adc")) score -= 80;

    return score;
}

static void sleep_ms_local(unsigned int ms)
{
    struct timespec req;
    req.tv_sec = ms / 1000;
    req.tv_nsec = (long)(ms % 1000) * 1000000L;

    while (nanosleep(&req, &req) < 0 && errno == EINTR)
        ;
}

static bool usb_audio_should_stop(void)
{
    bool stop;
    pthread_mutex_lock(&usb_audio_lock);
    stop = (usb_audio_stop != 0);
    pthread_mutex_unlock(&usb_audio_lock);
    return stop;
}

static bool strcasestr_simple(const char *haystack, const char *needle)
{
    size_t nlen = strlen(needle);
    if (nlen == 0)
        return true;

    for (size_t i = 0; haystack[i] != '\0'; i++)
    {
        size_t j = 0;
        while (needle[j] != '\0' && haystack[i + j] != '\0' &&
               (char)tolower((unsigned char)haystack[i + j]) == (char)tolower((unsigned char)needle[j]))
        {
            j++;
        }
        if (j == nlen)
            return true;
    }
    return false;
}

static int write_string_to_file(const char *path, const char *value)
{
    int fd = open(path, O_WRONLY | O_CLOEXEC);
    if (fd < 0)
    {
        printf("open %s failed: %s\n", path, strerror(errno));
        return -1;
    }

    size_t len = strlen(value);
    ssize_t n = write(fd, value, len);
    close(fd);
    if (n != (ssize_t)len)
    {
        printf("write %s failed: %s\n", path, strerror(errno));
        return -1;
    }
    return 0;
}

static void prime_codec_playback_mixer(void)
{
    snd_mixer_t *mixer = NULL;
    snd_mixer_elem_t *elem;
    int rc;
    int switch_count = 0;
    int volume_count = 0;
    int enum_count = 0;

    if (usb_codec_mixer_primed)
        return;

    rc = snd_mixer_open(&mixer, 0);
    if (rc < 0)
    {
        printf("snd_mixer_open failed: %s\n", snd_strerror(rc));
        return;
    }
    rc = snd_mixer_attach(mixer, "default");
    if (rc < 0)
    {
        printf("snd_mixer_attach default failed: %s\n", snd_strerror(rc));
        snd_mixer_close(mixer);
        return;
    }
    rc = snd_mixer_selem_register(mixer, NULL, NULL);
    if (rc < 0)
    {
        printf("snd_mixer_selem_register failed: %s\n", snd_strerror(rc));
        snd_mixer_close(mixer);
        return;
    }
    rc = snd_mixer_load(mixer);
    if (rc < 0)
    {
        printf("snd_mixer_load failed: %s\n", snd_strerror(rc));
        snd_mixer_close(mixer);
        return;
    }

    for (elem = snd_mixer_first_elem(mixer); elem; elem = snd_mixer_elem_next(elem))
    {
        if (!snd_mixer_selem_is_active(elem))
            continue;

        if (snd_mixer_selem_has_playback_switch(elem))
        {
            if (snd_mixer_selem_set_playback_switch_all(elem, 1) >= 0)
                switch_count++;
        }
        if (snd_mixer_selem_has_playback_volume(elem))
        {
            long vmin = 0;
            long vmax = 0;
            snd_mixer_selem_get_playback_volume_range(elem, &vmin, &vmax);
            if (snd_mixer_selem_set_playback_volume_all(elem, vmax) >= 0)
                volume_count++;
        }
        if (snd_mixer_selem_is_enumerated(elem))
        {
            unsigned int items = snd_mixer_selem_get_enum_items(elem);
            int best_idx = -1;
            int best_score = -100000;
            for (unsigned int i = 0; i < items; i++)
            {
                char item_name[128];
                if (snd_mixer_selem_get_enum_item_name(elem, i, sizeof(item_name), item_name) < 0)
                    continue;
                int score = enum_item_score(item_name);
                if (score > best_score)
                {
                    best_score = score;
                    best_idx = (int)i;
                }
            }

            if (best_idx >= 0 && best_score > 0)
            {
                int ok = 0;
                if (snd_mixer_selem_set_enum_item(elem, SND_MIXER_SCHN_FRONT_LEFT, (unsigned int)best_idx) >= 0)
                    ok = 1;
                if (snd_mixer_selem_set_enum_item(elem, SND_MIXER_SCHN_FRONT_RIGHT, (unsigned int)best_idx) >= 0)
                    ok = 1;
                if (ok)
                    enum_count++;
            }
        }
    }

    snd_mixer_close(mixer);
    usb_codec_mixer_primed = 1;
    printf("codec mixer primed: switches=%d volumes=%d enums=%d\n",
           switch_count, volume_count, enum_count);
}

static void trim_tail(char *s)
{
    size_t len = strlen(s);
    while (len > 0)
    {
        char c = s[len - 1];
        if (c == '\n' || c == '\r' || c == ' ' || c == '\t')
        {
            s[len - 1] = '\0';
            len--;
        }
        else
        {
            break;
        }
    }
}

static int read_string_from_file(const char *path, char *out, size_t out_size)
{
    int fd = open(path, O_RDONLY | O_CLOEXEC);
    ssize_t n;

    if (fd < 0)
    {
        printf("open %s failed: %s\n", path, strerror(errno));
        return -1;
    }

    n = read(fd, out, out_size - 1);
    close(fd);
    if (n < 0)
    {
        printf("read %s failed: %s\n", path, strerror(errno));
        return -1;
    }

    out[n] = '\0';
    trim_tail(out);
    return 0;
}

static int mkdir_if_missing(const char *path, mode_t mode)
{
    struct stat st;
    if (mkdir(path, mode) == 0)
        return 0;
    if (errno != EEXIST)
    {
        printf("mkdir %s failed: %s\n", path, strerror(errno));
        return -1;
    }
    if (stat(path, &st) < 0 || !S_ISDIR(st.st_mode))
    {
        printf("%s exists but is not a directory\n", path);
        return -1;
    }
    return 0;
}

static int ensure_symlink(const char *target, const char *linkpath)
{
    struct stat st;
    char current[128];
    ssize_t n;

    if (lstat(linkpath, &st) == 0)
    {
        if (!S_ISLNK(st.st_mode))
        {
            printf("%s exists but is not a symlink\n", linkpath);
            return -1;
        }

        n = readlink(linkpath, current, sizeof(current) - 1);
        if (n < 0)
        {
            printf("readlink %s failed: %s\n", linkpath, strerror(errno));
            return -1;
        }
        current[n] = '\0';
        if (strcmp(current, target) == 0)
            return 0;

        if (unlink(linkpath) < 0)
        {
            printf("unlink %s failed: %s\n", linkpath, strerror(errno));
            return -1;
        }
    }
    else if (errno != ENOENT)
    {
        printf("lstat %s failed: %s\n", linkpath, strerror(errno));
        return -1;
    }

    if (symlink(target, linkpath) < 0)
    {
        printf("symlink %s -> %s failed: %s\n", linkpath, target, strerror(errno));
        return -1;
    }

    return 0;
}

static int unlink_if_exists(const char *path)
{
    struct stat st;

    if (lstat(path, &st) < 0)
    {
        if (errno == ENOENT)
            return 0;
        printf("lstat %s failed: %s\n", path, strerror(errno));
        return -1;
    }

    if (unlink(path) < 0)
    {
        printf("unlink %s failed: %s\n", path, strerror(errno));
        return -1;
    }

    return 0;
}

static void remove_dir_if_exists(const char *path)
{
    if (rmdir(path) < 0)
    {
        if (errno == ENOENT)
            return;
        if (errno == ENOTEMPTY || errno == EBUSY)
            return;
        printf("rmdir %s failed: %s\n", path, strerror(errno));
    }
}

static void cleanup_usb_gadget_function_links(void)
{
    char uac1_link_path[192];
    char acm_link_path[192];

    snprintf(uac1_link_path, sizeof(uac1_link_path),
             "%s/configs/" USB_GADGET_CFG "/" USB_GADGET_UAC1_FUNC, USB_GADGET_PATH);
    (void)unlink_if_exists(uac1_link_path);

    snprintf(acm_link_path, sizeof(acm_link_path),
             "%s/configs/" USB_GADGET_CFG "/acm.usb0", USB_GADGET_PATH);
    (void)unlink_if_exists(acm_link_path);
}

static void cleanup_usb_gadget_function_dirs(void)
{
    char uac1_func_path[192];
    char acm_func_path[192];

    snprintf(uac1_func_path, sizeof(uac1_func_path),
             "%s/functions/" USB_GADGET_UAC1_FUNC, USB_GADGET_PATH);
    remove_dir_if_exists(uac1_func_path);

    snprintf(acm_func_path, sizeof(acm_func_path),
             "%s/functions/acm.usb0", USB_GADGET_PATH);
    remove_dir_if_exists(acm_func_path);
}

static void print_udc_list(void)
{
    DIR *dir = opendir("/sys/class/udc");
    struct dirent *de;

    if (!dir)
    {
        printf("opendir /sys/class/udc failed: %s\n", strerror(errno));
        return;
    }

    printf("available UDCs:");
    while ((de = readdir(dir)) != NULL)
    {
        if (strcmp(de->d_name, ".") == 0 || strcmp(de->d_name, "..") == 0)
            continue;
        printf(" %s", de->d_name);
    }
    printf("\n");
    closedir(dir);
}

static int ensure_usb_gadget_configfs(void)
{
    if (access(USB_GADGET_BASE, F_OK) == 0)
        return 0;

    if (mount("none", USB_CONFIGFS_PATH, "configfs", 0, NULL) < 0 && errno != EBUSY)
    {
        printf("mount configfs failed: %s\n", strerror(errno));
        return -1;
    }

    if (access(USB_GADGET_BASE, F_OK) < 0)
    {
        printf("usb gadget configfs path %s not found\n", USB_GADGET_BASE);
        return -1;
    }

    return 0;
}

static int bind_usb_gadget_udc(void)
{
    char udc_file[192];
    char current_udc[64];
    char udc_dir[192];

    snprintf(udc_file, sizeof(udc_file), "%s/UDC", USB_GADGET_PATH);
    if (read_string_from_file(udc_file, current_udc, sizeof(current_udc)) < 0)
        return -1;

    snprintf(udc_dir, sizeof(udc_dir), "/sys/class/udc/%s", USB_UDC_NAME);
    if (access(udc_dir, F_OK) < 0)
    {
        printf("UDC %s not found\n", USB_UDC_NAME);
        print_udc_list();
        return -1;
    }

    if (strcmp(current_udc, USB_UDC_NAME) == 0)
        return 0;

    if (current_udc[0] != '\0')
    {
        if (write_string_to_file(udc_file, "\n") < 0)
            return -1;
    }

    printf("binding UDC: %s\n", USB_UDC_NAME);
    if (write_string_to_file(udc_file, USB_UDC_NAME) < 0)
        return -1;

    return 0;
}

static int unbind_usb_gadget_udc(void)
{
    char udc_file[192];
    char current_udc[64];

    snprintf(udc_file, sizeof(udc_file), "%s/UDC", USB_GADGET_PATH);
    if (access(udc_file, F_OK) < 0)
        return 0;

    if (read_string_from_file(udc_file, current_udc, sizeof(current_udc)) < 0)
        return -1;
    if (current_udc[0] == '\0')
        return 0;

    if (write_string_to_file(udc_file, "\n") < 0)
        return -1;
    return 0;
}

static int find_uac_gadget_card(int *card_index)
{
    int card = -1;
    int err = snd_card_next(&card);
    if (err < 0)
    {
        printf("snd_card_next failed: %s\n", snd_strerror(err));
        return -1;
    }

    while (card >= 0)
    {
        char *name = NULL;
        if (snd_card_get_name(card, &name) >= 0 && name)
        {
            if (strcasestr_simple(name, "uac") || strcasestr_simple(name, "gadget"))
            {
                *card_index = card;
                free(name);
                return 0;
            }
            free(name);
        }
        if (snd_card_next(&card) < 0)
            break;
    }

    return -1;
}

static int find_codec_playback_device(int uac_card, char *dev_out, size_t dev_out_sz)
{
    (void)uac_card;
    snprintf(dev_out, dev_out_sz, USB_CODEC_PLAYBACK_DEV);
    return 0;
}

static int list_capture_pcm_devs_on_card(int card_index, usb_capture_dev_t *out, int max_count)
{
    snd_ctl_t *ctl = NULL;
    snd_pcm_info_t *pcminfo = NULL;
    char ctl_name[32];
    int dev = -1;
    int rc;
    int found = 0;
    int stored = 0;

    snprintf(ctl_name, sizeof(ctl_name), "hw:%d", card_index);
    rc = snd_ctl_open(&ctl, ctl_name, 0);
    if (rc < 0)
    {
        printf("snd_ctl_open %s failed: %s\n", ctl_name, snd_strerror(rc));
        return -1;
    }

    rc = snd_pcm_info_malloc(&pcminfo);
    if (rc < 0 || !pcminfo)
    {
        printf("snd_pcm_info_malloc failed: %s\n", snd_strerror(rc));
        snd_ctl_close(ctl);
        return -1;
    }

    while (1)
    {
        rc = snd_ctl_pcm_next_device(ctl, &dev);
        if (rc < 0 || dev < 0)
            break;

        snd_pcm_info_set_device(pcminfo, (unsigned int)dev);
        snd_pcm_info_set_subdevice(pcminfo, 0);
        snd_pcm_info_set_stream(pcminfo, SND_PCM_STREAM_CAPTURE);
        if (snd_ctl_pcm_info(ctl, pcminfo) >= 0)
        {
            found++;
            if (stored < max_count)
            {
                const char *n = snd_pcm_info_get_name(pcminfo);
                out[stored].dev = dev;
                if (n && n[0] != '\0')
                    snprintf(out[stored].name, sizeof(out[stored].name), "%s", n);
                else
                    snprintf(out[stored].name, sizeof(out[stored].name), "dev%d", dev);
                stored++;
            }
        }
    }

    snd_pcm_info_free(pcminfo);
    snd_ctl_close(ctl);
    if (found > stored)
    {
        printf("capture PCM device list truncated: found=%d stored=%d\n", found, stored);
    }
    return stored;
}

static int open_uac_capture_and_codec_playback(int card, int dev_idx,
                                               snd_pcm_t **uac_cap, snd_pcm_t **codec_play)
{
    int err;
    char dev_name[32];
    char codec_dev[32];

    *uac_cap = NULL;
    *codec_play = NULL;

    snprintf(dev_name, sizeof(dev_name), "hw:%d,%d", card, dev_idx);
    err = snd_pcm_open(uac_cap, dev_name, SND_PCM_STREAM_CAPTURE, 0);
    if (err < 0)
    {
        printf("open UAC capture %s failed: %s\n", dev_name, snd_strerror(err));
        return -1;
    }

    err = snd_pcm_set_params(*uac_cap,
                             SND_PCM_FORMAT_S16_LE,
                             SND_PCM_ACCESS_RW_INTERLEAVED,
                             USB_AUDIO_CHANNELS,
                             USB_AUDIO_RATE,
                             1,
                             200000);
    if (err < 0)
    {
        printf("set UAC capture params failed: %s\n", snd_strerror(err));
        snd_pcm_close(*uac_cap);
        *uac_cap = NULL;
        return -1;
    }

    err = snd_pcm_nonblock(*uac_cap, 1);
    if (err < 0)
    {
        printf("set UAC capture nonblock failed: %s\n", snd_strerror(err));
    }
    else
    {
        (void)snd_pcm_prepare(*uac_cap);
        (void)snd_pcm_start(*uac_cap);
    }

    if (find_codec_playback_device(card, codec_dev, sizeof(codec_dev)) < 0)
        snprintf(codec_dev, sizeof(codec_dev), "default");

    err = snd_pcm_open(codec_play, codec_dev, SND_PCM_STREAM_PLAYBACK, 0);
    if (err < 0)
    {
        printf("open codec playback %s failed: %s\n", codec_dev, snd_strerror(err));
        snd_pcm_close(*uac_cap);
        *uac_cap = NULL;
        return -1;
    }

    err = snd_pcm_set_params(*codec_play,
                             SND_PCM_FORMAT_S16_LE,
                             SND_PCM_ACCESS_RW_INTERLEAVED,
                             USB_AUDIO_CHANNELS,
                             USB_AUDIO_RATE,
                             1,
                             200000);
    if (err < 0)
    {
        printf("set codec playback params failed: %s\n", snd_strerror(err));
        snd_pcm_close(*codec_play);
        snd_pcm_close(*uac_cap);
        *codec_play = NULL;
        *uac_cap = NULL;
        return -1;
    }

    err = snd_pcm_nonblock(*codec_play, 1);
    if (err < 0)
    {
        printf("set codec playback nonblock failed: %s\n", snd_strerror(err));
    }

    prime_codec_playback_mixer();
    printf("usb audio bridge ready: hw:%d,%d -> %s\n", card, dev_idx, codec_dev);
    return 0;
}

static int wait_codec_playback_ready(unsigned int timeout_ms)
{
    unsigned int waited = 0;
    snd_pcm_t *pcm = NULL;
    int err;

    while (waited < timeout_ms)
    {
        err = snd_pcm_open(&pcm, USB_CODEC_PLAYBACK_DEV, SND_PCM_STREAM_PLAYBACK, 0);
        if (err >= 0)
        {
            snd_pcm_close(pcm);
            return 0;
        }

        if (err != -EBUSY)
        {
            printf("probe codec playback %s failed: %s\n",
                   USB_CODEC_PLAYBACK_DEV, snd_strerror(err));
        }
        sleep_ms_local(100);
        waited += 100;
    }

    return -1;
}

static int recover_uac_capture_stream(snd_pcm_t *uac_cap, int errcode)
{
    int r;

    if (!uac_cap)
        return -1;

    if (errcode == -EINTR || errcode == -EPIPE || errcode == -ESTRPIPE)
    {
        r = snd_pcm_recover(uac_cap, errcode, 1);
        return (r < 0) ? -1 : 0;
    }

    return -1;
}

static void *usb_audio_bridge_entry(void *arg)
{
    snd_pcm_t *uac_cap = NULL;
    snd_pcm_t *codec_play = NULL;
    usb_capture_dev_t cap_devs[USB_MAX_CAPTURE_DEVS];
    int16_t *buffer = NULL;
    size_t samples = USB_AUDIO_FRAME * USB_AUDIO_CHANNELS;
    unsigned long long frames_in = 0;
    unsigned long long frames_out = 0;
    unsigned long long samples_total = 0;
    unsigned long long samples_nonzero = 0;
    int peak_abs = 0;
    unsigned int loop_ticks = 0;
    int open_fail_count = 0;
    int read_fail_streak = 0;
    unsigned int eio_wait_log_tick = 0;
    unsigned int silent_chunk_streak = 0;
    int uac_card = -1;
    int cap_count = 0;
    int cap_pick = 0;
    int cur_cap_dev = -1;

    (void)arg;
    buffer = (int16_t *)malloc(samples * sizeof(int16_t));
    if (!buffer)
    {
        printf("usb audio bridge alloc failed\n");
        return NULL;
    }

    while (!usb_audio_should_stop())
    {
        if (!uac_cap || !codec_play)
        {
            if (cap_count <= 0)
            {
                if (find_uac_gadget_card(&uac_card) < 0)
                {
                    open_fail_count++;
                    if ((open_fail_count % 10) == 1)
                        printf("usb audio bridge waiting for UAC card... (fail=%d)\n", open_fail_count);
                    sleep_ms_local(300);
                    continue;
                }

                cap_count = list_capture_pcm_devs_on_card(uac_card, cap_devs, USB_MAX_CAPTURE_DEVS);
                if (cap_count <= 0)
                {
                    open_fail_count++;
                    if ((open_fail_count % 10) == 1)
                        printf("no capture PCM on UAC card %d yet (fail=%d)\n", uac_card, open_fail_count);
                    sleep_ms_local(300);
                    continue;
                }

                cap_pick = 0;
                printf("UAC card %d capture devices:", uac_card);
                for (int i = 0; i < cap_count && i < USB_MAX_CAPTURE_DEVS; i++)
                    printf(" [%d]=%d:%s", i, cap_devs[i].dev, cap_devs[i].name);
                printf("\n");
            }

            if (uac_cap) { snd_pcm_close(uac_cap); uac_cap = NULL; }
            if (codec_play) { snd_pcm_close(codec_play); codec_play = NULL; }
            if (cap_pick >= cap_count)
                cap_pick = 0;
            cur_cap_dev = cap_devs[cap_pick].dev;

            if (open_uac_capture_and_codec_playback(uac_card, cur_cap_dev, &uac_cap, &codec_play) < 0)
            {
                open_fail_count++;
                if ((open_fail_count % 10) == 1)
                {
                    printf("usb audio bridge waiting for devices... card=%d dev=%d fail=%d\n",
                           uac_card, cur_cap_dev, open_fail_count);
                }
                cap_count = 0;
                sleep_ms_local(300);
                continue;
            }
            open_fail_count = 0;
            silent_chunk_streak = 0;
        }

        snd_pcm_sframes_t nread = snd_pcm_readi(uac_cap, buffer, USB_AUDIO_FRAME);
        if (nread == -EAGAIN)
        {
            sleep_ms_local(2);
            continue;
        }
        if (nread < 0)
        {
            if (recover_uac_capture_stream(uac_cap, (int)nread) < 0)
            {
                read_fail_streak++;
                eio_wait_log_tick++;
                if ((eio_wait_log_tick % 25) == 1)
                {
                    printf("uac capture read failed, reopen: %s\n", snd_strerror((int)nread));
                }
                if (uac_cap) { snd_pcm_close(uac_cap); uac_cap = NULL; }
                if (codec_play) { snd_pcm_close(codec_play); codec_play = NULL; }
                read_fail_streak = 0;
                sleep_ms_local(100);
            }
            else
            {
                read_fail_streak = 0;
            }
            continue;
        }
        read_fail_streak = 0;
        int chunk_peak_abs = 0;
        unsigned int chunk_nonzero = 0;
        for (snd_pcm_sframes_t i = 0; i < nread * USB_AUDIO_CHANNELS; i++)
        {
            int s = (int)buffer[i];
            int a = (s < 0) ? -s : s;
            if (a > peak_abs)
                peak_abs = a;
            if (a > chunk_peak_abs)
                chunk_peak_abs = a;
            if (s != 0)
            {
                samples_nonzero++;
                chunk_nonzero++;
            }
            samples_total++;
        }

        if (chunk_peak_abs <= 8 || chunk_nonzero == 0)
            silent_chunk_streak++;
        else
            silent_chunk_streak = 0;

        if (silent_chunk_streak > 600 && cap_count > 1)
        {
            int next_pick = (cap_pick + 1) % cap_count;
            printf("uac source seems silent on dev=%d, switch to dev=%d (%s)\n",
                   cur_cap_dev, cap_devs[next_pick].dev, cap_devs[next_pick].name);
            cap_pick = next_pick;
            silent_chunk_streak = 0;
            if (uac_cap) { snd_pcm_close(uac_cap); uac_cap = NULL; }
            if (codec_play) { snd_pcm_close(codec_play); codec_play = NULL; }
            sleep_ms_local(30);
            continue;
        }

        frames_in += (unsigned long long)nread;

        snd_pcm_sframes_t offset = 0;
        while (offset < nread && !usb_audio_should_stop())
        {
            snd_pcm_sframes_t nw = snd_pcm_writei(codec_play,
                                                  buffer + offset * USB_AUDIO_CHANNELS,
                                                  (snd_pcm_uframes_t)(nread - offset));
            if (nw == -EAGAIN)
            {
                sleep_ms_local(2);
                continue;
            }
            if (nw < 0)
            {
                if (snd_pcm_recover(codec_play, (int)nw, 1) < 0)
                {
                    printf("codec playback recover failed: %s\n", snd_strerror((int)nw));
                    snd_pcm_close(uac_cap);
                    snd_pcm_close(codec_play);
                    uac_cap = NULL;
                    codec_play = NULL;
                    break;
                }
                continue;
            }
            frames_out += (unsigned long long)nw;
            offset += nw;
        }

        loop_ticks++;
        if ((loop_ticks % 200) == 0)
        {
            printf("usb audio bridge stats: src=hw:%d,%d in=%llu out=%llu peak=%d nz=%llu/%llu silent=%u\n",
                   uac_card, cur_cap_dev, frames_in, frames_out, peak_abs,
                   samples_nonzero, samples_total, silent_chunk_streak);
        }
    }

    if (uac_cap) snd_pcm_close(uac_cap);
    if (codec_play) snd_pcm_close(codec_play);
    free(buffer);
    return NULL;
}

static int usb_audio_bridge_start(void)
{
    int rc = 0;

    pthread_mutex_lock(&usb_audio_lock);
    if (usb_audio_running)
    {
        pthread_mutex_unlock(&usb_audio_lock);
        return 0;
    }
    usb_audio_stop = 0;
    rc = pthread_create(&usb_audio_thread, NULL, usb_audio_bridge_entry, NULL);
    if (rc == 0)
        usb_audio_running = 1;
    pthread_mutex_unlock(&usb_audio_lock);

    if (rc != 0)
    {
        printf("start usb audio bridge failed: %s\n", strerror(rc));
        return -1;
    }

    return 0;
}

static void usb_audio_bridge_stop(void)
{
    pthread_t tid;
    int need_join = 0;

    pthread_mutex_lock(&usb_audio_lock);
    if (usb_audio_running)
    {
        usb_audio_stop = 1;
        tid = usb_audio_thread;
        need_join = 1;
    }
    pthread_mutex_unlock(&usb_audio_lock);

    if (need_join)
    {
        pthread_join(tid, NULL);
        pthread_mutex_lock(&usb_audio_lock);
        usb_audio_running = 0;
        pthread_mutex_unlock(&usb_audio_lock);
    }
}

static int configure_usb_gadget_composite(void)
{
    char path[192];
    char uac1_link_path[192];
    char acm_link_path[192];

    if (ensure_usb_gadget_configfs() < 0)
        return -1;

    if (mkdir_if_missing(USB_GADGET_PATH, 0755) < 0)
        return -1;

    if (unbind_usb_gadget_udc() < 0)
        return -1;
    sleep_ms_local(30);

    cleanup_usb_gadget_function_links();
    cleanup_usb_gadget_function_dirs();

    snprintf(path, sizeof(path), "%s/strings", USB_GADGET_PATH);
    if (mkdir_if_missing(path, 0755) < 0)
        return -1;
    snprintf(path, sizeof(path), "%s/strings/" USB_GADGET_LANG, USB_GADGET_PATH);
    if (mkdir_if_missing(path, 0755) < 0)
        return -1;

    snprintf(path, sizeof(path), "%s/configs", USB_GADGET_PATH);
    if (mkdir_if_missing(path, 0755) < 0)
        return -1;
    snprintf(path, sizeof(path), "%s/configs/" USB_GADGET_CFG, USB_GADGET_PATH);
    if (mkdir_if_missing(path, 0755) < 0)
        return -1;
    snprintf(path, sizeof(path), "%s/configs/" USB_GADGET_CFG "/strings", USB_GADGET_PATH);
    if (mkdir_if_missing(path, 0755) < 0)
        return -1;
    snprintf(path, sizeof(path), "%s/configs/" USB_GADGET_CFG "/strings/" USB_GADGET_LANG, USB_GADGET_PATH);
    if (mkdir_if_missing(path, 0755) < 0)
        return -1;

    snprintf(path, sizeof(path), "%s/functions", USB_GADGET_PATH);
    if (mkdir_if_missing(path, 0755) < 0)
        return -1;
#if USB_ENABLE_ACM_CONSOLE
    snprintf(path, sizeof(path), "%s/functions/" USB_GADGET_ACM_FUNC, USB_GADGET_PATH);
    if (mkdir_if_missing(path, 0755) < 0)
        return -1;
#endif
    snprintf(path, sizeof(path), "%s/functions/" USB_GADGET_UAC1_FUNC, USB_GADGET_PATH);
    if (mkdir_if_missing(path, 0755) < 0)
    {
        printf("create UAC1 function failed, check CONFIG_USB_CONFIGFS_F_UAC1\n");
        return -1;
    }

    snprintf(path, sizeof(path), "%s/idVendor", USB_GADGET_PATH);
    if (write_string_to_file(path, "0x1d6b") < 0) return -1;
    snprintf(path, sizeof(path), "%s/idProduct", USB_GADGET_PATH);
    if (write_string_to_file(path, "0x0109") < 0) return -1;
    snprintf(path, sizeof(path), "%s/bcdUSB", USB_GADGET_PATH);
    if (write_string_to_file(path, "0x0200") < 0) return -1;
    snprintf(path, sizeof(path), "%s/bcdDevice", USB_GADGET_PATH);
    if (write_string_to_file(path, "0x0100") < 0) return -1;
    snprintf(path, sizeof(path), "%s/bMaxPacketSize0", USB_GADGET_PATH);
    if (write_string_to_file(path, "64") < 0) return -1;
    snprintf(path, sizeof(path), "%s/bDeviceClass", USB_GADGET_PATH);
    if (write_string_to_file(path, "0x00") < 0) return -1;
    snprintf(path, sizeof(path), "%s/bDeviceSubClass", USB_GADGET_PATH);
    if (write_string_to_file(path, "0x00") < 0) return -1;
    snprintf(path, sizeof(path), "%s/bDeviceProtocol", USB_GADGET_PATH);
    if (write_string_to_file(path, "0x00") < 0) return -1;

    snprintf(path, sizeof(path), "%s/strings/" USB_GADGET_LANG "/serialnumber", USB_GADGET_PATH);
    if (write_string_to_file(path, "UAC1PB0002") < 0) return -1;
    snprintf(path, sizeof(path), "%s/strings/" USB_GADGET_LANG "/manufacturer", USB_GADGET_PATH);
    if (write_string_to_file(path, "F1C200S") < 0) return -1;
    snprintf(path, sizeof(path), "%s/strings/" USB_GADGET_LANG "/product", USB_GADGET_PATH);
    if (write_string_to_file(path, "USB Audio Playback") < 0) return -1;

    snprintf(path, sizeof(path), "%s/configs/" USB_GADGET_CFG "/MaxPower", USB_GADGET_PATH);
    if (write_string_to_file(path, "250") < 0) return -1;
    snprintf(path, sizeof(path), "%s/configs/" USB_GADGET_CFG "/bmAttributes", USB_GADGET_PATH);
    if (write_string_to_file(path, "0x80") < 0) return -1;
    snprintf(path, sizeof(path), "%s/configs/" USB_GADGET_CFG "/strings/" USB_GADGET_LANG "/configuration", USB_GADGET_PATH);
    if (write_string_to_file(path, "USB-Audio-Playback") < 0) return -1;

    /* UAC1 stable profile for Windows: stereo 48kHz 16-bit in/out */
    snprintf(path, sizeof(path), "%s/functions/" USB_GADGET_UAC1_FUNC "/p_chmask", USB_GADGET_PATH);
    if (write_string_to_file(path, "0x3") < 0) return -1;
    snprintf(path, sizeof(path), "%s/functions/" USB_GADGET_UAC1_FUNC "/p_srate", USB_GADGET_PATH);
    if (write_string_to_file(path, "48000") < 0) return -1;
    snprintf(path, sizeof(path), "%s/functions/" USB_GADGET_UAC1_FUNC "/p_ssize", USB_GADGET_PATH);
    if (write_string_to_file(path, "2") < 0) return -1;
    snprintf(path, sizeof(path), "%s/functions/" USB_GADGET_UAC1_FUNC "/c_chmask", USB_GADGET_PATH);
    if (write_string_to_file(path, "0x3") < 0) return -1;
    snprintf(path, sizeof(path), "%s/functions/" USB_GADGET_UAC1_FUNC "/c_srate", USB_GADGET_PATH);
    if (write_string_to_file(path, "48000") < 0) return -1;
    snprintf(path, sizeof(path), "%s/functions/" USB_GADGET_UAC1_FUNC "/c_ssize", USB_GADGET_PATH);
    if (write_string_to_file(path, "2") < 0) return -1;

#if USB_ENABLE_ACM_CONSOLE
    snprintf(acm_link_path, sizeof(acm_link_path), "%s/configs/" USB_GADGET_CFG "/" USB_GADGET_ACM_FUNC, USB_GADGET_PATH);
    if (ensure_symlink(USB_GADGET_PATH "/functions/" USB_GADGET_ACM_FUNC, acm_link_path) < 0)
        return -1;
#else
    snprintf(acm_link_path, sizeof(acm_link_path), "%s/configs/" USB_GADGET_CFG "/acm.usb0", USB_GADGET_PATH);
    if (unlink_if_exists(acm_link_path) < 0)
        return -1;
#endif

    snprintf(uac1_link_path, sizeof(uac1_link_path), "%s/configs/" USB_GADGET_CFG "/" USB_GADGET_UAC1_FUNC, USB_GADGET_PATH);
    if (ensure_symlink(USB_GADGET_PATH "/functions/" USB_GADGET_UAC1_FUNC, uac1_link_path) < 0)
        return -1;

    if (bind_usb_gadget_udc() < 0)
        return -1;

    return 0;
}

#if USB_ENABLE_ACM_CONSOLE
static int wait_for_tty_node(const char *path, unsigned int timeout_ms)
{
    const unsigned int sleep_ms_step = 20;
    unsigned int waited = 0;

    while (waited < timeout_ms)
    {
        if (access(path, F_OK) == 0)
            return 0;
        sleep_ms_local(sleep_ms_step);
        waited += sleep_ms_step;
    }

    return -1;
}

static void set_tty_sane_mode(int fd)
{
    struct termios tio;

    if (tcgetattr(fd, &tio) < 0)
        return;

    tio.c_iflag = BRKINT | ICRNL | IXON | IMAXBEL;
    tio.c_oflag = OPOST | ONLCR;
    tio.c_cflag = CREAD | CS8 | HUPCL | CLOCAL;
    tio.c_lflag = ISIG | ICANON | ECHO | ECHOE | ECHOK | IEXTEN;
    cfsetispeed(&tio, B115200);
    cfsetospeed(&tio, B115200);
    tio.c_cc[VMIN] = 1;
    tio.c_cc[VTIME] = 0;

    (void)tcsetattr(fd, TCSANOW, &tio);
}

static pid_t spawn_raw_shell_on_tty(const char *tty_path)
{
    pid_t pid = fork();
    if (pid < 0)
    {
        printf("fork failed for tty shell: %s\n", strerror(errno));
        return -1;
    }

    if (pid == 0)
    {
        int fd;
        setsid();
        fd = open(tty_path, O_RDWR | O_NOCTTY);
        if (fd < 0)
            _exit(127);
#ifdef TIOCSCTTY
        ioctl(fd, TIOCSCTTY, 0);
#endif
        set_tty_sane_mode(fd);
        dup2(fd, STDIN_FILENO);
        dup2(fd, STDOUT_FILENO);
        dup2(fd, STDERR_FILENO);
        if (fd > STDERR_FILENO)
            close(fd);
        setenv("TERM", "vt100", 1);
        setenv("PS1", "# ", 0);
        (void)write(STDOUT_FILENO, "\r\nPC Mode shell ready\r\n", 23);
        execl("/bin/sh", "sh", "-i", (char *)NULL);
        _exit(127);
    }

    return pid;
}

static pid_t spawn_console_on_tty(void)
{
    pid_t pid;
    int status;
    pid_t rc;

    printf("usb console start: raw /bin/sh on %s\n", USB_GSERIAL_TTY);
    pid = spawn_raw_shell_on_tty(USB_GSERIAL_TTY);
    if (pid <= 0)
        return -1;

    sleep_ms_local(150);
    rc = waitpid(pid, &status, WNOHANG);
    if (rc == 0)
    {
        printf("usb console started, pid=%d\n", (int)pid);
        return pid;
    }

    printf("usb console exited early, status=%d\n", status);
    return -1;
}

static void stop_usb_console(pid_t *pid_ptr)
{
    int status;
    pid_t pid = *pid_ptr;

    if (pid <= 0)
        return;

    if (kill(pid, SIGHUP) < 0 && errno != ESRCH)
    {
        printf("failed to stop usb console pid=%d: %s\n", (int)pid, strerror(errno));
    }

    for (int i = 0; i < 20; i++)
    {
        pid_t rc = waitpid(pid, &status, WNOHANG);
        if (rc == pid)
        {
            *pid_ptr = 0;
            return;
        }
        if (rc < 0 && errno == ECHILD)
        {
            *pid_ptr = 0;
            return;
        }
        sleep_ms_local(100);
    }

    if (kill(pid, SIGKILL) == 0)
        (void)waitpid(pid, &status, 0);
    *pid_ptr = 0;
}
#else
static void stop_usb_console(pid_t *pid_ptr)
{
    if (pid_ptr)
        *pid_ptr = 0;
}
#endif

int app_usb_enter_pc_mode(pid_t *console_pid)
{
    pid_t pid = 0;

    usb_codec_mixer_primed = 0;

    if (write_string_to_file(USB_BRIGHTNESS_PATH, "0") < 0)
        return -1;

    if (write_string_to_file(USB_MODE_PATH, "peripheral") < 0)
    {
        (void)write_string_to_file(USB_BRIGHTNESS_PATH, "1");
        return -1;
    }

    if (configure_usb_gadget_composite() < 0)
    {
        (void)write_string_to_file(USB_MODE_PATH, "host");
        (void)write_string_to_file(USB_BRIGHTNESS_PATH, "1");
        return -1;
    }

#if USB_ENABLE_ACM_CONSOLE
    if (wait_for_tty_node(USB_GSERIAL_TTY, 6000) < 0)
    {
        printf("tty gadget node %s not ready\n", USB_GSERIAL_TTY);
        (void)unbind_usb_gadget_udc();
        (void)write_string_to_file(USB_MODE_PATH, "host");
        (void)write_string_to_file(USB_BRIGHTNESS_PATH, "1");
        return -1;
    }

    pid = spawn_console_on_tty();
    if (pid <= 0)
    {
        (void)unbind_usb_gadget_udc();
        (void)write_string_to_file(USB_MODE_PATH, "host");
        (void)write_string_to_file(USB_BRIGHTNESS_PATH, "1");
        return -1;
    }
#endif

    if (wait_codec_playback_ready(4000) < 0)
    {
        printf("codec playback %s is still busy before bridge start\n", USB_CODEC_PLAYBACK_DEV);
        stop_usb_console(&pid);
        (void)unbind_usb_gadget_udc();
        (void)write_string_to_file(USB_MODE_PATH, "host");
        (void)write_string_to_file(USB_BRIGHTNESS_PATH, "1");
        return -1;
    }

    if (usb_audio_bridge_start() < 0)
    {
        stop_usb_console(&pid);
        (void)unbind_usb_gadget_udc();
        (void)write_string_to_file(USB_MODE_PATH, "host");
        (void)write_string_to_file(USB_BRIGHTNESS_PATH, "1");
        return -1;
    }

    *console_pid = pid;
    return 0;
}

int app_usb_exit_pc_mode(pid_t *console_pid)
{
    int rc = 0;

    usb_audio_bridge_stop();
    stop_usb_console(console_pid);
    if (unbind_usb_gadget_udc() < 0)
        rc = -1;
    sleep_ms_local(30);
    cleanup_usb_gadget_function_links();
    cleanup_usb_gadget_function_dirs();

    if (write_string_to_file(USB_BRIGHTNESS_PATH, "1") < 0)
        rc = -1;
    if (write_string_to_file(USB_MODE_PATH, "host") < 0)
        rc = -1;

    return rc;
}
