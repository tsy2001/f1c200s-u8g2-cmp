#ifndef _APP_USB_H_
#define _APP_USB_H_

#include <sys/types.h>

/* Enter USB peripheral UAC1 audio mode (host playback path is used by app). */
int app_usb_enter_pc_mode(pid_t *console_pid);
/* Stop USB peripheral mode and return USB back to host mode. */
int app_usb_exit_pc_mode(pid_t *console_pid);

#endif
