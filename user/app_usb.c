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
#include <ctype.h>
#include <alsa/asoundlib.h>
#include <usbg/usbg.h>
#include <usbg/function/uac2.h>
#include <usbg/function/serial.h>
#include "app_usb.h"
#include "cmp_module_ctl.h"

#define USB_GSERIAL_TTY "/dev/ttyGS0"
#define USB_UDC_NAME "musb-hdrc.1.auto"
#define USB_CONFIGFS_PATH "/sys/kernel/config"

#define USB_GADGET_BASE "/sys/kernel/config/usb_gadget"
#define USB_GADGET_NAME "g1"
#define USB_GADGET_PATH USB_GADGET_BASE "/" USB_GADGET_NAME
#define USB_GADGET_CFG "c.1"

#define USB_GADGET_UAC_FUNC "uac2.usb0"
#define USB_GADGET_ACM_FUNC "acm.usb0"
#define USB_GADGET_UAC_LINK "uac2"
#define USB_GADGET_ACM_LINK "acm.GS0"

#define USB_UDC_CURRENT_SPEED_PATH "/sys/class/udc/" USB_UDC_NAME "/current_speed"

#define USB_AUDIO_RATE_FS 48000
#define USB_AUDIO_RATE_HS 96000
#define USB_AUDIO_RATE_FS_STR "48000"
#define USB_AUDIO_RATE_HS_STR "96000"
#define USB_AUDIO_FORMAT "S24_3LE"
#define USB_AUDIO_SAMPLE_BYTES 3

#define USB_MAX_CAPTURE_DEVS 8
#define USB_CODEC_PLAYBACK_DEV "plughw:1,0"
#define USB_UAC_CAPTURE_DEV "hw:3,0"

#define USB_ALSALOOP_PATH "/usr/bin/alsaloop"
#define USB_ALSALOOP_LATENCY_US "100000"

static pthread_t usb_audio_thread;
static pthread_mutex_t usb_audio_lock = PTHREAD_MUTEX_INITIALIZER;
static int usb_audio_running = 0;
static int usb_audio_stop = 0;
static int usb_audio_rate = USB_AUDIO_RATE_FS;
static const char *usb_audio_rate_str = USB_AUDIO_RATE_FS_STR;

static pthread_t usb_console_thread;
static pthread_mutex_t usb_console_lock = PTHREAD_MUTEX_INITIALIZER;
static int usb_console_running = 0;
static int usb_console_stop = 0;
static pid_t usb_console_pid = 0;

typedef struct
{
    int dev;
    char name[64];
} usb_capture_dev_t;

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

static int write_int_to_file(const char *path, int value)
{
    char buf[32];

    snprintf(buf, sizeof(buf), "%d", value);
    return write_string_to_file(path, buf);
}

static int set_usb_switch_level(bool high)
{
    if (cmp_module_set_usb_switch(high) < 0)
    {
        printf("set usb switch %s failed: %s\n",
               high ? "high" : "low", strerror(errno));
        return -1;
    }

    return 0;
}

static int set_usb_mode_host(void)
{
    if (cmp_module_set_usb_mode(CMP_MODULE_USB_MODE_HOST) < 0)
    {
        printf("set usb mode host failed: %s\n", strerror(errno));
        return -1;
    }

    return 0;
}

static int set_usb_mode_peripheral(void)
{
    if (cmp_module_set_usb_mode(CMP_MODULE_USB_MODE_PERIPHERAL) < 0)
    {
        printf("set usb mode peripheral failed: %s\n", strerror(errno));
        return -1;
    }

    return 0;
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
             "%s/configs/" USB_GADGET_CFG "/" USB_GADGET_UAC_LINK, USB_GADGET_PATH);
    (void)unlink_if_exists(uac1_link_path);

    snprintf(acm_link_path, sizeof(acm_link_path),
             "%s/configs/" USB_GADGET_CFG "/" USB_GADGET_ACM_LINK, USB_GADGET_PATH);
    (void)unlink_if_exists(acm_link_path);
}

static void cleanup_usb_gadget_function_dirs(void)
{
    char uac1_func_path[192];
    char acm_func_path[192];

    snprintf(uac1_func_path, sizeof(uac1_func_path),
             "%s/functions/" USB_GADGET_UAC_FUNC, USB_GADGET_PATH);
    remove_dir_if_exists(uac1_func_path);

    snprintf(acm_func_path, sizeof(acm_func_path),
             "%s/functions/" USB_GADGET_ACM_FUNC, USB_GADGET_PATH);
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

static int write_usb_function_int_attr(const char *func, const char *attr,
                                       int value)
{
    char path[192];

    snprintf(path, sizeof(path), "%s/functions/%s/%s",
             USB_GADGET_PATH, func, attr);
    return write_int_to_file(path, value);
}

static void set_usb_audio_profile(bool high_speed)
{
    if (high_speed)
    {
        usb_audio_rate = USB_AUDIO_RATE_HS;
        usb_audio_rate_str = USB_AUDIO_RATE_HS_STR;
    }
    else
    {
        usb_audio_rate = USB_AUDIO_RATE_FS;
        usb_audio_rate_str = USB_AUDIO_RATE_FS_STR;
    }

    printf("usb audio profile: %s %dHz 24bit stereo\n",
           high_speed ? "high-speed" : "full-speed safe", usb_audio_rate);
}

static int configure_usb_audio_function_attrs(void)
{
    struct {
        const char *name;
        int value;
    } attrs[] = {
        { "c_chmask", 0x3 },
        { "c_srate", USB_AUDIO_RATE_FS },
        { "c_ssize", USB_AUDIO_SAMPLE_BYTES },
        { "fs_c_srate", USB_AUDIO_RATE_FS },
        { "hs_c_srate", USB_AUDIO_RATE_HS },
    };
    size_t i;

    for (i = 0; i < sizeof(attrs) / sizeof(attrs[0]); i++)
    {
        if (write_usb_function_int_attr(USB_GADGET_UAC_FUNC,
                                        attrs[i].name,
                                        attrs[i].value) < 0)
        {
            printf("configure UAC2 attr %s failed\n", attrs[i].name);
            return -1;
        }
    }

    return 0;
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

static int wait_usb_current_speed(bool *high_speed, unsigned int timeout_ms)
{
    unsigned int waited = 0;
    char speed[64];

    while (waited < timeout_ms)
    {
        if (read_string_from_file(USB_UDC_CURRENT_SPEED_PATH,
                                  speed, sizeof(speed)) == 0)
        {
            if (strcasestr_simple(speed, "high"))
            {
                *high_speed = true;
                printf("usb current speed: %s\n", speed);
                return 0;
            }
            if (strcasestr_simple(speed, "full"))
            {
                *high_speed = false;
                printf("usb current speed: %s\n", speed);
                return 0;
            }
            if (speed[0] != '\0' && !strcasestr_simple(speed, "unknown"))
            {
                printf("usb current speed unhandled: %s, use full-speed safe profile\n",
                       speed);
                *high_speed = false;
                return 0;
            }
        }

        sleep_ms_local(100);
        waited += 100;
    }

    printf("usb current speed not ready, use full-speed safe profile\n");
    *high_speed = false;
    return -1;
}

static bool usb_udc_is_connected(void)
{
    char speed[64];

    if (read_string_from_file(USB_UDC_CURRENT_SPEED_PATH, speed, sizeof(speed)) < 0)
        return false;

    if (speed[0] == '\0' || strcasestr_simple(speed, "unknown") ||
        strcasestr_simple(speed, "not attached"))
        return false;

    return true;
}

static void stop_child_process(pid_t *pid_ptr, const char *name)
{
    int status;
    pid_t pid = *pid_ptr;

    if (pid <= 0)
        return;

    if (kill(pid, SIGTERM) < 0 && errno != ESRCH)
        printf("failed to stop %s pid=%d: %s\n", name, (int)pid, strerror(errno));

    for (int i = 0; i < 20; i++)
    {
        pid_t rc = waitpid(pid, &status, WNOHANG);
        if (rc == pid || (rc < 0 && errno == ECHILD))
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

static int start_alsaloop_process(char *cap_dev, pid_t *pid_out)
{
    pid_t pid;

    pid = fork();
    if (pid < 0)
    {
        printf("fork alsaloop failed: %s\n", strerror(errno));
        return -1;
    }

    if (pid == 0)
    {
        execl(USB_ALSALOOP_PATH, "alsaloop",
              "-C", cap_dev,
              "-P", USB_CODEC_PLAYBACK_DEV,
              "-f", USB_AUDIO_FORMAT,
              "-c", "2",
              "-r", usb_audio_rate_str,
              "-t", USB_ALSALOOP_LATENCY_US,
              "-S", "1",
              "-b",
              (char *)NULL);
        _exit(127);
    }

    *pid_out = pid;
    printf("usb audio alsaloop started: %s -> %s pid=%d\n",
           cap_dev, USB_CODEC_PLAYBACK_DEV, (int)pid);
    return 0;
}

static void *usb_audio_alsaloop_entry(void *arg)
{
    pid_t loop_pid = 0;
    int fail_count = 0;
    bool high_speed = false;

    (void)arg;

    while (!usb_audio_should_stop())
    {
        int status;
        pid_t rc;

        if (!usb_udc_is_connected())
        {
            if (loop_pid > 0)
            {
                stop_child_process(&loop_pid, "alsaloop");
            }
            sleep_ms_local(200);
            continue;
        }

        if (loop_pid > 0)
        {
            rc = waitpid(loop_pid, &status, WNOHANG);
            if (rc == 0)
            {
                sleep_ms_local(200);
                continue;
            }
            printf("usb audio alsaloop exited: pid=%d status=%d\n",
                   (int)loop_pid, status);
            loop_pid = 0;
            sleep_ms_local(300);
            continue;
        }

        if (wait_usb_current_speed(&high_speed, 50) == 0)
            set_usb_audio_profile(high_speed);
        else
            set_usb_audio_profile(false);

        if (start_alsaloop_process(USB_UAC_CAPTURE_DEV, &loop_pid) < 0)
        {
            fail_count++;
            sleep_ms_local(500);
            continue;
        }

        fail_count = 0;
    }

    stop_child_process(&loop_pid, "alsaloop");
    return NULL;
}

static int usb_audio_loop_start(void)
{
    int rc = 0;

    pthread_mutex_lock(&usb_audio_lock);
    if (usb_audio_running)
    {
        pthread_mutex_unlock(&usb_audio_lock);
        return 0;
    }
    usb_audio_stop = 0;
    if (access(USB_ALSALOOP_PATH, X_OK) < 0)
    {
        pthread_mutex_unlock(&usb_audio_lock);
        printf("usb audio requires %s: %s\n", USB_ALSALOOP_PATH, strerror(errno));
        return -1;
    }

    printf("usb audio using alsaloop backend\n");
    rc = pthread_create(&usb_audio_thread, NULL, usb_audio_alsaloop_entry, NULL);
    if (rc == 0)
        usb_audio_running = 1;
    pthread_mutex_unlock(&usb_audio_lock);

    if (rc != 0)
    {
        printf("start usb audio loop failed: %s\n", strerror(rc));
        return -1;
    }

    return 0;
}

static void usb_audio_loop_stop(void)
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
    usbg_state *state = NULL;
    usbg_gadget *gadget = NULL;
    usbg_config *config = NULL;
    usbg_function *uac = NULL;
    usbg_function *acm = NULL;
    usbg_udc *udc = NULL;
    int ret;
    const char *stage = "unknown";
    struct usbg_gadget_attrs gadget_attrs = {
        .bcdUSB = 0x0200,
        .bDeviceClass = 0xEF,
        .bDeviceSubClass = 0x02,
        .bDeviceProtocol = 0x01,
        .bMaxPacketSize0 = 64,
        .idVendor = 0x1d6b,
        .idProduct = 0x0109,
        .bcdDevice = 0x0104,
    };
    struct usbg_gadget_strs gadget_strs = {
        .manufacturer = "Allwinner",
        .product = "F1C200S CMP Module",
        .serial = "TSY20010608",
    };
    struct usbg_config_attrs config_attrs = {
        .bmAttributes = 0x80,
        .bMaxPower = 250,
    };
    struct usbg_config_strs config_strs = {
        .configuration = "UAC2-ACM",
    };

    if (ensure_usb_gadget_configfs() < 0)
        return -1;

    stage = "usbg_init";
    ret = usbg_init(USB_CONFIGFS_PATH, &state);
    if (ret != USBG_SUCCESS)
    {
        printf("libusbgx %s failed: %s: %s\n",
               stage, usbg_error_name(ret), usbg_strerror(ret));
        return -1;
    }

    (void)unbind_usb_gadget_udc();
    sleep_ms_local(30);

    gadget = usbg_get_gadget(state, USB_GADGET_NAME);
    if (gadget)
    {
        (void)usbg_disable_gadget(gadget);
        (void)usbg_rm_gadget(gadget, USBG_RM_RECURSE);
    }

    stage = "create gadget";
    ret = usbg_create_gadget(state, USB_GADGET_NAME,
                             &gadget_attrs, &gadget_strs, &gadget);
    if (ret != USBG_SUCCESS)
        goto usbg_fail;

    stage = "create config";
    ret = usbg_create_config(gadget, 1, "c",
                             &config_attrs, &config_strs, &config);
    if (ret != USBG_SUCCESS)
        goto usbg_fail;

    stage = "create UAC2 function";
    ret = usbg_create_function(gadget, USBG_F_UAC2, "usb0", NULL, &uac);
    if (ret != USBG_SUCCESS)
        goto usbg_fail;

    stage = "configure UAC2 attrs";
    if (configure_usb_audio_function_attrs() < 0)
    {
        ret = USBG_ERROR_IO;
        goto usbg_fail;
    }

    stage = "add UAC2 function";
    ret = usbg_add_config_function(config, USB_GADGET_UAC_LINK, uac);
    if (ret != USBG_SUCCESS)
        goto usbg_fail;

    stage = "create ACM function";
    ret = usbg_create_function(gadget, USBG_F_ACM, "usb0", NULL, &acm);
    if (ret != USBG_SUCCESS)
        goto usbg_fail;

    stage = "add ACM function";
    ret = usbg_add_config_function(config, USB_GADGET_ACM_LINK, acm);
    if (ret != USBG_SUCCESS)
        goto usbg_fail;

    stage = "get UDC";
    udc = usbg_get_udc(state, USB_UDC_NAME);
    if (!udc)
    {
        printf("UDC %s not found\n", USB_UDC_NAME);
        print_udc_list();
        ret = USBG_ERROR_NOT_FOUND;
        goto usbg_fail;
    }

    stage = "enable gadget";
    ret = usbg_enable_gadget(gadget, udc);
    if (ret != USBG_SUCCESS)
        goto usbg_fail;

    usbg_cleanup(state);
    return 0;

usbg_fail:
    printf("libusbgx %s failed: %s: %s\n",
           stage, usbg_error_name(ret), usbg_strerror(ret));
    if (gadget)
    {
        (void)usbg_disable_gadget(gadget);
        (void)usbg_rm_gadget(gadget, USBG_RM_RECURSE);
    }
    usbg_cleanup(state);
    return -1;
}

static int configure_usb_gadget_for_current_speed(void)
{
    bool high_speed = false;

    set_usb_audio_profile(false);
    if (configure_usb_gadget_composite() < 0)
        return -1;

    if (wait_usb_current_speed(&high_speed, 2500) == 0 && high_speed)
        set_usb_audio_profile(true);

    return 0;
}

static bool usb_console_should_stop(void)
{
    bool stop;

    pthread_mutex_lock(&usb_console_lock);
    stop = (usb_console_stop != 0);
    pthread_mutex_unlock(&usb_console_lock);
    return stop;
}

static void usb_console_set_pid(pid_t pid)
{
    pthread_mutex_lock(&usb_console_lock);
    usb_console_pid = pid;
    pthread_mutex_unlock(&usb_console_lock);
}

static pid_t usb_console_get_pid(void)
{
    pid_t pid;

    pthread_mutex_lock(&usb_console_lock);
    pid = usb_console_pid;
    pthread_mutex_unlock(&usb_console_lock);
    return pid;
}

static const char *tty_dev_name(const char *tty_path)
{
    const char *slash = strrchr(tty_path, '/');

    return slash ? slash + 1 : tty_path;
}

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
        ioctl(fd, TIOCSCTTY, 1);
#endif
        (void)tcsetpgrp(fd, getpid());
        set_tty_sane_mode(fd);
        dup2(fd, STDIN_FILENO);
        dup2(fd, STDOUT_FILENO);
        dup2(fd, STDERR_FILENO);
        if (fd > STDERR_FILENO)
            close(fd);
        setenv("TERM", "vt100", 1);
        setenv("PS1", "# ", 1);
        (void)write(STDOUT_FILENO, "\r\nPC Mode shell ready\r\n", 23);
        execl("/bin/sh", "sh", "-i", (char *)NULL);
        _exit(127);
    }

    return pid;
}

static pid_t spawn_getty_shell_on_tty(const char *tty_path)
{
    const char *tty_name = tty_dev_name(tty_path);
    pid_t pid = fork();

    if (pid < 0)
    {
        printf("fork failed for tty getty: %s\n", strerror(errno));
        return -1;
    }

    if (pid == 0)
    {
        setsid();
        setenv("TERM", "vt100", 1);
        setenv("PS1", "# ", 1);
        execl("/sbin/getty", "getty",
              "-L", "-i", "-n", "-l", "/bin/sh",
              "115200", tty_name, "vt100", (char *)NULL);
        _exit(127);
    }

    return pid;
}

static pid_t spawn_console_on_tty(void)
{
    pid_t pid;
    int status;
    pid_t rc;

    printf("usb console start: getty root shell on %s\n", USB_GSERIAL_TTY);
    pid = spawn_getty_shell_on_tty(USB_GSERIAL_TTY);
    if (pid <= 0)
        return -1;

    sleep_ms_local(150);
    rc = waitpid(pid, &status, WNOHANG);
    if (rc == 0)
    {
        printf("usb console started, pid=%d\n", (int)pid);
        return pid;
    }

    printf("usb getty console exited early, status=%d, fallback raw shell\n", status);
    pid = spawn_raw_shell_on_tty(USB_GSERIAL_TTY);
    if (pid <= 0)
        return -1;

    sleep_ms_local(150);
    rc = waitpid(pid, &status, WNOHANG);
    if (rc == 0)
    {
        printf("usb raw console started, pid=%d\n", (int)pid);
        return pid;
    }

    printf("usb raw console exited early, status=%d\n", status);
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

static void *usb_console_entry(void *arg)
{
    pid_t pid = 0;
    int fail_count = 0;

    (void)arg;

    while (!usb_console_should_stop())
    {
        int status;
        pid_t rc;

        if (!usb_udc_is_connected())
        {
            if (pid > 0)
            {
                stop_usb_console(&pid);
                usb_console_set_pid(0);
            }
            sleep_ms_local(200);
            continue;
        }

        if (pid > 0)
        {
            rc = waitpid(pid, &status, WNOHANG);
            if (rc == 0)
            {
                sleep_ms_local(300);
                continue;
            }

            printf("usb console exited: pid=%d status=%d\n", (int)pid, status);
            pid = 0;
            usb_console_set_pid(0);
            sleep_ms_local(300);
            continue;
        }

        if (wait_for_tty_node(USB_GSERIAL_TTY, 1000) < 0)
        {
            fail_count++;
            if ((fail_count % 5) == 1)
                printf("usb console waiting for %s... (fail=%d)\n",
                       USB_GSERIAL_TTY, fail_count);
            continue;
        }

        pid = spawn_console_on_tty();
        if (pid <= 0)
        {
            fail_count++;
            sleep_ms_local(500);
            continue;
        }

        fail_count = 0;
        usb_console_set_pid(pid);
    }

    if (pid > 0)
    {
        stop_usb_console(&pid);
        usb_console_set_pid(0);
    }

    return NULL;
}

static int usb_console_loop_start(pid_t *console_pid)
{
    int rc;
    unsigned int waited = 0;

    pthread_mutex_lock(&usb_console_lock);
    if (usb_console_running)
    {
        if (console_pid)
            *console_pid = usb_console_pid;
        pthread_mutex_unlock(&usb_console_lock);
        return 0;
    }

    usb_console_stop = 0;
    usb_console_pid = 0;
    rc = pthread_create(&usb_console_thread, NULL, usb_console_entry, NULL);
    if (rc == 0)
        usb_console_running = 1;
    pthread_mutex_unlock(&usb_console_lock);

    if (rc != 0)
    {
        printf("start usb console monitor failed: %s\n", strerror(rc));
        return -1;
    }

    while (waited < 2000)
    {
        pid_t pid = usb_console_get_pid();

        if (pid > 0)
        {
            if (console_pid)
                *console_pid = pid;
            return 0;
        }

        sleep_ms_local(50);
        waited += 50;
    }

    printf("usb console monitor started, shell not ready yet\n");
    if (console_pid)
        *console_pid = 0;
    return 0;
}

static void usb_console_loop_stop(pid_t *console_pid)
{
    pthread_t tid;
    int need_join = 0;

    pthread_mutex_lock(&usb_console_lock);
    if (usb_console_running)
    {
        usb_console_stop = 1;
        tid = usb_console_thread;
        need_join = 1;
    }
    pthread_mutex_unlock(&usb_console_lock);

    if (need_join)
    {
        pthread_join(tid, NULL);
        pthread_mutex_lock(&usb_console_lock);
        usb_console_running = 0;
        usb_console_stop = 0;
        usb_console_pid = 0;
        pthread_mutex_unlock(&usb_console_lock);
    }

    if (console_pid)
        *console_pid = 0;
}

int app_usb_enter_pc_mode(pid_t *console_pid)
{
    pid_t pid = 0;

    if (set_usb_switch_level(false) < 0)
        return -1;

    if (set_usb_mode_peripheral() < 0)
    {
        set_usb_switch_level(true);
        return -1;
    }

    if (configure_usb_gadget_for_current_speed() < 0)
    {
        set_usb_mode_host();
        set_usb_switch_level(true);
        return -1;
    }

    if (wait_for_tty_node(USB_GSERIAL_TTY, 6000) < 0)
    {
        printf("tty gadget node %s not ready\n", USB_GSERIAL_TTY);
        unbind_usb_gadget_udc();
        set_usb_mode_host();
        set_usb_switch_level(true);
        return -1;
    }

    if (usb_console_loop_start(&pid) < 0)
    {
        unbind_usb_gadget_udc();
        set_usb_mode_host();
        set_usb_switch_level(true);
        return -1;
    }

    if (wait_codec_playback_ready(4000) < 0)
    {
        printf("codec playback %s is still busy before loop start\n", USB_CODEC_PLAYBACK_DEV);
        unbind_usb_gadget_udc();
        usb_console_loop_stop(&pid);
        set_usb_mode_host();
        set_usb_switch_level(true);
        return -1;
    }

    if (usb_audio_loop_start() < 0)
    {
        unbind_usb_gadget_udc();
        usb_console_loop_stop(&pid);
        set_usb_mode_host();
        set_usb_switch_level(true);
        return -1;
    }

    *console_pid = pid;
    return 0;
}

int app_usb_exit_pc_mode(pid_t *console_pid)
{
    int rc = 0;

    if (unbind_usb_gadget_udc() < 0)
        rc = -1;
    sleep_ms_local(30);
    usb_audio_loop_stop();
    usb_console_loop_stop(console_pid);
    cleanup_usb_gadget_function_links();
    cleanup_usb_gadget_function_dirs();

    if (set_usb_switch_level(true) < 0)
        rc = -1;
    if (set_usb_mode_host() < 0)
        rc = -1;

    return rc;
}
