#ifndef _CMP_MODULE_CTL_H_
#define _CMP_MODULE_CTL_H_

#include <stdbool.h>
#include <stdint.h>

#define CMP_MODULE_GPIO_CODEC_MUTE (1u << 0)
#define CMP_MODULE_GPIO_USB_SWITCH (1u << 1)

#define CMP_MODULE_USB_MODE_HOST 1u
#define CMP_MODULE_USB_MODE_PERIPHERAL 2u
#define CMP_MODULE_USB_MODE_OTG 3u

int cmp_module_open(void);
void cmp_module_close(void);

int cmp_module_set_gpios(uint32_t mask, uint32_t value);
int cmp_module_get_gpios(uint32_t mask, uint32_t *value);

int cmp_module_set_codec_mute(bool high);
int cmp_module_set_usb_switch(bool high);

int cmp_module_set_usb_mode(uint32_t mode);

#endif
