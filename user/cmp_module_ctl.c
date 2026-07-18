#include "cmp_module_ctl.h"

#include <errno.h>
#include <fcntl.h>
#include <pthread.h>
#include <stdint.h>
#include <sys/ioctl.h>
#include <unistd.h>

#define CMP_MODULE_DEVICE_PATH "/dev/cmp-module"
#define CMP_MODULE_IOC_MAGIC 'c'
#define CMP_MODULE_GPIO_ALL \
    (CMP_MODULE_GPIO_CODEC_MUTE | CMP_MODULE_GPIO_USB_SWITCH)

struct cmp_module_gpio_state {
    uint32_t mask;
    uint32_t value;
};

#define CMP_MODULE_IOC_SET_GPIOS \
    _IOW(CMP_MODULE_IOC_MAGIC, 0x01, struct cmp_module_gpio_state)
#define CMP_MODULE_IOC_GET_GPIOS \
    _IOWR(CMP_MODULE_IOC_MAGIC, 0x02, struct cmp_module_gpio_state)
#define CMP_MODULE_IOC_SET_USB_MODE \
    _IOW(CMP_MODULE_IOC_MAGIC, 0x03, uint32_t)

static pthread_mutex_t cmp_module_lock = PTHREAD_MUTEX_INITIALIZER;
static int cmp_module_fd = -1;

static int cmp_module_open_locked(void)
{
    if (cmp_module_fd >= 0)
        return 0;

    cmp_module_fd = open(CMP_MODULE_DEVICE_PATH, O_RDWR | O_CLOEXEC);
    if (cmp_module_fd < 0)
        return -1;

    return 0;
}

int cmp_module_open(void)
{
    int ret;

    pthread_mutex_lock(&cmp_module_lock);
    ret = cmp_module_open_locked();
    pthread_mutex_unlock(&cmp_module_lock);

    return ret;
}

void cmp_module_close(void)
{
    pthread_mutex_lock(&cmp_module_lock);
    if (cmp_module_fd >= 0)
    {
        close(cmp_module_fd);
        cmp_module_fd = -1;
    }
    pthread_mutex_unlock(&cmp_module_lock);
}

int cmp_module_set_gpios(uint32_t mask, uint32_t value)
{
    struct cmp_module_gpio_state state;
    int ret;

    if (mask == 0 || (mask & ~CMP_MODULE_GPIO_ALL))
    {
        errno = EINVAL;
        return -1;
    }

    state.mask = mask;
    state.value = value & mask;

    pthread_mutex_lock(&cmp_module_lock);
    ret = cmp_module_open_locked();
    if (ret == 0)
        ret = ioctl(cmp_module_fd, CMP_MODULE_IOC_SET_GPIOS, &state);
    pthread_mutex_unlock(&cmp_module_lock);

    return ret < 0 ? -1 : 0;
}

int cmp_module_get_gpios(uint32_t mask, uint32_t *value)
{
    struct cmp_module_gpio_state state;
    int ret;

    if (value == NULL || (mask & ~CMP_MODULE_GPIO_ALL))
    {
        errno = EINVAL;
        return -1;
    }

    state.mask = mask;
    state.value = 0;

    pthread_mutex_lock(&cmp_module_lock);
    ret = cmp_module_open_locked();
    if (ret == 0)
        ret = ioctl(cmp_module_fd, CMP_MODULE_IOC_GET_GPIOS, &state);
    pthread_mutex_unlock(&cmp_module_lock);

    if (ret < 0)
        return -1;

    *value = state.value;
    return 0;
}

int cmp_module_set_codec_mute(bool high)
{
    return cmp_module_set_gpios(CMP_MODULE_GPIO_CODEC_MUTE,
                                high ? CMP_MODULE_GPIO_CODEC_MUTE : 0);
}

int cmp_module_set_usb_switch(bool high)
{
    return cmp_module_set_gpios(CMP_MODULE_GPIO_USB_SWITCH,
                                high ? CMP_MODULE_GPIO_USB_SWITCH : 0);
}

int cmp_module_set_usb_mode(uint32_t mode)
{
    int ret;

    if (mode != CMP_MODULE_USB_MODE_HOST &&
        mode != CMP_MODULE_USB_MODE_PERIPHERAL &&
        mode != CMP_MODULE_USB_MODE_OTG)
    {
        errno = EINVAL;
        return -1;
    }

    pthread_mutex_lock(&cmp_module_lock);
    ret = cmp_module_open_locked();
    if (ret == 0)
        ret = ioctl(cmp_module_fd, CMP_MODULE_IOC_SET_USB_MODE, &mode);
    pthread_mutex_unlock(&cmp_module_lock);

    return ret < 0 ? -1 : 0;
}

int cmp_module_set_usb_mode_host(void)
{
    return cmp_module_set_usb_mode(CMP_MODULE_USB_MODE_HOST);
}

int cmp_module_set_usb_mode_peripheral(void)
{
    return cmp_module_set_usb_mode(CMP_MODULE_USB_MODE_PERIPHERAL);
}

int cmp_module_set_usb_mode_otg(void)
{
    return cmp_module_set_usb_mode(CMP_MODULE_USB_MODE_OTG);
}
