#ifndef _POSIX_C_SOURCE
#define _POSIX_C_SOURCE 200809L
#endif

#include <sys/stat.h>
#include <sys/wait.h>
#include <sys/mount.h>
#include <stdio.h>
#include <string.h>
#include <errno.h>
#include <signal.h>
#include <time.h>
#include <dirent.h>
#include <fcntl.h>
#include <unistd.h>
#include <pthread.h>
#include <stdbool.h>
#include <usbg/usbg.h>
#include "app_mtp.h"
#include "cmp_module_ctl.h"

#define USB_CONFIGFS_PATH "/sys/kernel/config"
#define USB_GADGET_BASE "/sys/kernel/config/usb_gadget"
#define USB_GADGET_NAME "g1"
#define USB_GADGET_PATH USB_GADGET_BASE "/" USB_GADGET_NAME
#define USB_GADGET_CFG "c.1"
#define USB_GADGET_MTP_FUNC "ffs.mtp"
#define USB_GADGET_MTP_LINK "mtp"

#define USB_UDC_NAME "musb-hdrc.1.auto"
#define USB_UMTPRD_PATH "/usr/sbin/umtprd"
#define USB_UMTPRD_CONF "/etc/umtprd/umtprd.conf"
#define USB_MTP_FFS_DIR "/dev/ffs-mtp"
#define USB_PC_MODE_MARKER "/tmp/cmp-usb-pc-mode"

static pthread_mutex_t app_mtp_lock = PTHREAD_MUTEX_INITIALIZER;
static int app_mtp_running = 0;
static pid_t app_mtp_pid = 0;
static pid_t app_mtp_daemon = 0;

static void sleep_ms_local(unsigned int ms)
{
    struct timespec req;

    req.tv_sec = ms / 1000;
    req.tv_nsec = (long)(ms % 1000) * 1000000L;
    while (nanosleep(&req, &req) < 0 && errno == EINTR)
        ;
}

static int write_string_to_file(const char *path, const char *value)
{
    int fd = open(path, O_WRONLY | O_CLOEXEC);
    if (fd < 0)
        return -1;

    size_t len = strlen(value);
    ssize_t n = write(fd, value, len);
    close(fd);
    return n == (ssize_t)len ? 0 : -1;
}

static int ensure_usb_gadget_configfs(void)
{
    if (access(USB_GADGET_BASE, F_OK) == 0)
        return 0;
    if (mount("none", USB_CONFIGFS_PATH, "configfs", 0, NULL) < 0 && errno != EBUSY)
        return -1;
    return access(USB_GADGET_BASE, F_OK) == 0 ? 0 : -1;
}

static int unlink_if_exists(const char *path)
{
    if (unlink(path) < 0 && errno != ENOENT)
        return -1;
    return 0;
}

static int create_marker_file(const char *path)
{
    int fd = open(path, O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, 0644);
    if (fd < 0)
        return -1;
    close(fd);
    return 0;
}

static void remove_marker_file(const char *path)
{
    (void)unlink_if_exists(path);
}

static int set_usb_switch_level(bool high)
{
    return cmp_module_set_usb_switch(high);
}

static int set_usb_mode_host(void)
{
    return cmp_module_set_usb_mode(CMP_MODULE_USB_MODE_HOST);
}

static int set_usb_mode_peripheral(void)
{
    return cmp_module_set_usb_mode(CMP_MODULE_USB_MODE_PERIPHERAL);
}

static int unbind_usb_gadget_udc(void)
{
    char udc_file[192];

    snprintf(udc_file, sizeof(udc_file), "%s/UDC", USB_GADGET_PATH);
    if (access(udc_file, F_OK) < 0)
        return 0;
    return write_string_to_file(udc_file, "\n");
}

static void remove_dir_if_exists(const char *path)
{
    if (rmdir(path) < 0 && errno != ENOENT && errno != ENOTEMPTY && errno != EBUSY)
        printf("rmdir %s failed: %s\n", path, strerror(errno));
}

static void cleanup_usb_gadget_function_links(void)
{
    char path[192];

    snprintf(path, sizeof(path), "%s/configs/" USB_GADGET_CFG "/" USB_GADGET_MTP_LINK,
             USB_GADGET_PATH);
    (void)unlink_if_exists(path);
}

static void cleanup_usb_gadget_function_dirs(void)
{
    char path[192];

    snprintf(path, sizeof(path), "%s/functions/" USB_GADGET_MTP_FUNC, USB_GADGET_PATH);
    remove_dir_if_exists(path);
}

static int mount_usb_mtp_functionfs(void)
{
    if (mkdir(USB_MTP_FFS_DIR, 0755) < 0 && errno != EEXIST)
        return -1;
    if (mount("mtp", USB_MTP_FFS_DIR, "functionfs", 0, NULL) < 0)
        return -1;
    return 0;
}

static void unmount_usb_mtp_functionfs(void)
{
    (void)umount(USB_MTP_FFS_DIR);
}

static int usb_mtp_enabled(void)
{
    return access(USB_UMTPRD_PATH, X_OK) == 0 &&
           access(USB_UMTPRD_CONF, R_OK) == 0;
}

static pid_t spawn_umtprd_daemon(void)
{
    pid_t pid = fork();

    if (pid < 0)
        return -1;
    if (pid == 0)
    {
        setsid();
        execl(USB_UMTPRD_PATH, "umtprd",
              "-conf:" USB_UMTPRD_CONF,
              (char *)NULL);
        _exit(127);
    }
    return pid;
}

static void stop_child_process(pid_t *pid_ptr)
{
    int status;
    pid_t pid = *pid_ptr;

    if (pid <= 0)
        return;

    if (kill(pid, SIGTERM) < 0 && errno != ESRCH)
        printf("failed to stop mtp pid=%d: %s\n", (int)pid, strerror(errno));

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

static int process_has_functionfs_ep0(pid_t pid)
{
    char dir_path[64];
    DIR *dir;
    struct dirent *de;
    char link_path[128];
    char target[256];
    ssize_t len;

    snprintf(dir_path, sizeof(dir_path), "/proc/%d/fd", (int)pid);
    dir = opendir(dir_path);
    if (!dir)
        return 0;

    while ((de = readdir(dir)) != NULL)
    {
        if (de->d_name[0] == '.')
            continue;

        snprintf(link_path, sizeof(link_path), "%s/%s", dir_path, de->d_name);
        len = readlink(link_path, target, sizeof(target) - 1);
        if (len < 0)
            continue;
        target[len] = '\0';
        if (strstr(target, USB_MTP_FFS_DIR "/ep0") != NULL)
        {
            closedir(dir);
            return 1;
        }
    }

    closedir(dir);
    return 0;
}

static int wait_for_umtprd_ready(pid_t pid, unsigned int timeout_ms)
{
    unsigned int waited = 0;

    while (waited < timeout_ms)
    {
        if (process_has_functionfs_ep0(pid))
            return 0;
        sleep_ms_local(50);
        waited += 50;
    }

    return -1;
}

static int setup_mtp_gadget(void)
{
    usbg_state *state = NULL;
    usbg_gadget *gadget = NULL;
    usbg_config *config = NULL;
    usbg_function *mtp = NULL;
    usbg_udc *udc = NULL;
    int ret;
    const char *stage = "unknown";
    struct usbg_gadget_attrs gadget_attrs = {
        .bcdUSB = 0x0200,
        .bDeviceClass = 0x00,
        .bDeviceSubClass = 0x00,
        .bDeviceProtocol = 0x00,
        .bMaxPacketSize0 = 64,
        .idVendor = 0x1d6b,
        .idProduct = 0x010a,
        .bcdDevice = 0x0100,
    };
    struct usbg_gadget_strs gadget_strs = {
        .manufacturer = "F1C200S",
        .product = "USB MTP",
        .serial = "CMP0004",
    };
    struct usbg_config_attrs config_attrs = {
        .bmAttributes = 0x80,
        .bMaxPower = 250,
    };
    struct usbg_config_strs config_strs = {
        .configuration = "MTP",
    };

    if (ensure_usb_gadget_configfs() < 0)
        return -1;

    ret = usbg_init(USB_CONFIGFS_PATH, &state);
    if (ret != USBG_SUCCESS)
        return -1;

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
        goto fail;

    stage = "create config";
    ret = usbg_create_config(gadget, 1, "c",
                             &config_attrs, &config_strs, &config);
    if (ret != USBG_SUCCESS)
        goto fail;

    stage = "create mtp function";
    ret = usbg_create_function(gadget, USBG_F_FFS, "mtp", NULL, &mtp);
    if (ret != USBG_SUCCESS)
        goto fail;

    stage = "add mtp function";
    ret = usbg_add_config_function(config, USB_GADGET_MTP_LINK, mtp);
    if (ret != USBG_SUCCESS)
        goto fail;

    stage = "mount functionfs";
    if (mount_usb_mtp_functionfs() < 0)
        goto fail;

    stage = "spawn umtprd";
    app_mtp_daemon = spawn_umtprd_daemon();
    if (app_mtp_daemon <= 0)
        goto fail;

    stage = "wait umtprd ready";
    if (wait_for_umtprd_ready(app_mtp_daemon, 5000) < 0)
        goto fail;

    stage = "get UDC";
    udc = usbg_get_udc(state, USB_UDC_NAME);
    if (!udc)
        goto fail;

    stage = "enable gadget";
    ret = usbg_enable_gadget(gadget, udc);
    if (ret != USBG_SUCCESS)
        goto fail;

    usbg_cleanup(state);
    return 0;

fail:
    if (gadget)
    {
        (void)usbg_disable_gadget(gadget);
        (void)usbg_rm_gadget(gadget, USBG_RM_RECURSE);
    }
    cleanup_usb_gadget_function_links();
    cleanup_usb_gadget_function_dirs();
    unmount_usb_mtp_functionfs();
    if (app_mtp_daemon > 0)
    {
        stop_child_process(&app_mtp_daemon);
    }
    usbg_cleanup(state);
    (void)stage;
    return -1;
}

int app_mtp_enter_mode(void)
{
    if (!usb_mtp_enabled())
        return -1;

    pthread_mutex_lock(&app_mtp_lock);
    if (app_mtp_running)
    {
        pthread_mutex_unlock(&app_mtp_lock);
        return 0;
    }
    pthread_mutex_unlock(&app_mtp_lock);

    if (set_usb_switch_level(false) < 0)
        return -1;
    if (set_usb_mode_peripheral() < 0)
    {
        (void)set_usb_switch_level(true);
        return -1;
    }

    if (create_marker_file(USB_PC_MODE_MARKER) < 0)
    {
        (void)set_usb_mode_host();
        (void)set_usb_switch_level(true);
        return -1;
    }

    if (setup_mtp_gadget() < 0)
    {
        remove_marker_file(USB_PC_MODE_MARKER);
        (void)set_usb_mode_host();
        (void)set_usb_switch_level(true);
        return -1;
    }

    pthread_mutex_lock(&app_mtp_lock);
    app_mtp_pid = app_mtp_daemon;
    app_mtp_running = 1;
    pthread_mutex_unlock(&app_mtp_lock);

    return 0;
}

int app_mtp_exit_mode(void)
{
    pid_t pid;

    pthread_mutex_lock(&app_mtp_lock);
    pid = app_mtp_pid;
    app_mtp_pid = 0;
    app_mtp_running = 0;
    pthread_mutex_unlock(&app_mtp_lock);

    if (pid > 0)
        stop_child_process(&pid);

    (void)unbind_usb_gadget_udc();
    cleanup_usb_gadget_function_links();
    cleanup_usb_gadget_function_dirs();
    unmount_usb_mtp_functionfs();
    remove_marker_file(USB_PC_MODE_MARKER);
    (void)set_usb_mode_host();
    (void)set_usb_switch_level(true);
    return 0;
}
