#ifndef _POSIX_C_SOURCE
#define _POSIX_C_SOURCE 199309L
#endif

#include <sys/time.h>
#include <time.h>
#include <sys/ioctl.h>
#include <linux/input.h>
#include <pthread.h>
#include <poll.h>
#include <fcntl.h>
#include <unistd.h>
#include <stdio.h>
#include <signal.h>
#include <arpa/inet.h>
#include <string.h>
#include "led.h"
#include "u8g2port.h"
#include "app_button.h"
#include "app_cdplayer.h"
#include "beep.h"

#define ADC_BUTTON_DEVICE "/dev/input/event0"
#define IR_BUTTON_DEVICE "/dev/input/event1"

pthread_t button_read_thread;
pthread_t button_handle_thread;
pthread_t button_ir_thread;

led_t *led_handle = NULL;
KeyDetector key[3];
extern APP_CDIO app_cdio;

uint32_t GetSystemTime(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts); // 使用单调时钟，不受系统时间修改影响
    return (uint32_t)((ts.tv_sec * 1000) + (ts.tv_nsec / 1000000));
}

void KeyDetector_Init(KeyDetector *detector)
{
    detector->state = KEY_STATE_IDLE;
    detector->pressTime = 0;
    detector->releaseTime = 0;
    detector->keyPressed = false;
    detector->keyPressedLast = false;
    detector->event = KEY_EVENT_NONE;
}

// 更新按键状态
void KeyDetector_Update(KeyDetector *detector, unsigned long currentTime)
{
    detector->keyPressedLast = detector->keyPressed;
    detector->keyPressed = detector->keyRaw;
    detector->event = KEY_EVENT_NONE; // 每次调用时重置事件

    switch (detector->state)
    {
    case KEY_STATE_IDLE:
        if (detector->keyPressed && !detector->keyPressedLast)
        {
            // 检测到按键按下(消抖处理)
            detector->pressTime = currentTime;
            detector->state = KEY_STATE_PRESS;
        }
        break;

    case KEY_STATE_PRESS:
        if (!detector->keyPressed && detector->keyPressedLast)
        {
            // 按键释放
            if (currentTime - detector->pressTime < DEBOUNCE_TIME)
            {
                // 消抖处理，认为是误触发
                detector->state = KEY_STATE_IDLE;
            }
            else
            {
                // 有效释放
                detector->releaseTime = currentTime;
                detector->state = KEY_STATE_RELEASE;
            }
        }
        else if (detector->keyPressed &&
                 (currentTime - detector->pressTime >= LONG_PRESS_TIME))
        {
            // 长按事件
            detector->event = KEY_EVENT_LONG_PRESS;
            detector->state = KEY_STATE_LONG_PRESS;
        }
        break;

    case KEY_STATE_RELEASE:
        if (currentTime - detector->releaseTime > DOUBLE_CLICK_TIME)
        {
            // 超时，确定为单击
            detector->event = KEY_EVENT_CLICK;
            detector->state = KEY_STATE_IDLE;
        }
        else if (detector->keyPressed && !detector->keyPressedLast)
        {
            // 在双击时间内再次按下，进入等待双击状态
            detector->pressTime = currentTime;
            detector->state = KEY_STATE_WAIT_DOUBLE;
        }
        break;

    case KEY_STATE_WAIT_DOUBLE:
        if (!detector->keyPressed && detector->keyPressedLast)
        {
            // 第二次释放
            if (currentTime - detector->pressTime < DEBOUNCE_TIME)
            {
                // 消抖处理
                detector->state = KEY_STATE_WAIT_DOUBLE;
            }
            else
            {
                // 确定为双击
                detector->event = KEY_EVENT_DOUBLE;
                detector->state = KEY_STATE_IDLE;
            }
        }
        else if (currentTime - detector->pressTime >= LONG_PRESS_TIME)
        {
            // 第二次按下时间过长，认为是长按
            detector->event = KEY_EVENT_LONG_PRESS;
            detector->state = KEY_STATE_LONG_PRESS;
        }
        break;

    case KEY_STATE_LONG_PRESS:
        if (!detector->keyPressed && detector->keyPressedLast)
        {
            // 长按释放
            detector->state = KEY_STATE_IDLE;
        }
        break;
    }
}

// 获取按键事件
KeyEvent KeyDetector_GetEvent(KeyDetector *detector)
{
    KeyEvent event = detector->event;
    detector->event = KEY_EVENT_NONE; // 读取后清除事件
    return event;
}

void app_button_start(void)
{
    int err = 0;
    led_handle = led_new();
    if (led_handle == NULL)
    {
        printf("Failed to create LED handle\n");
        return;
    }

    err = led_open(led_handle, "user");
    if (err != 0)
    {
        printf("Failed to open LED device\n");
        led_free(led_handle);
        led_handle = NULL;
        return;
    }
    
    pthread_create(&button_read_thread, NULL, button_read_entry, NULL);
    pthread_create(&button_handle_thread, NULL, button_handl_entry, NULL);
    pthread_create(&button_ir_thread, NULL, button_ir_entry, NULL);
}

void app_button_wait(void)
{
    pthread_join(button_read_thread, NULL);
    pthread_join(button_handle_thread, NULL);
    pthread_join(button_ir_thread, NULL);
}

void *button_handl_entry(void *arg)
{
    unsigned long tick_now = 0;

    KeyDetector_Init(&key[0]);
    KeyDetector_Init(&key[1]);
    KeyDetector_Init(&key[2]);

    while (1)
    {
        tick_now = GetSystemTime();

        KeyDetector_Update(&key[0], tick_now);
        KeyDetector_Update(&key[1], tick_now);
        KeyDetector_Update(&key[2], tick_now);

        KeyEvent event[3];

        event[0] = KeyDetector_GetEvent(&key[0]);
        switch (event[0])
        {
        case KEY_EVENT_CLICK:
            printf("Left Click detected\n");
            app_cdio_adjust_volume(-0.05f);
            break;
        case KEY_EVENT_DOUBLE:
            app_cdio_set_prev();
            printf("Left Double click detected\n");
            break;
        case KEY_EVENT_LONG_PRESS:
            printf("Left Long press detected\n");
            break;
        default:
            break;
        }

        event[1] = KeyDetector_GetEvent(&key[1]);
        switch (event[1])
        {
        case KEY_EVENT_CLICK:
            app_cdio_toggle_stop();
            printf("Mid Click detected\n");
            break;
        case KEY_EVENT_DOUBLE:
            printf("Mid Double click detected\n");
            break;
        case KEY_EVENT_LONG_PRESS:
            app_cdio_set_eject();
            printf("Mid Long press detected\n");
            break;
        default:
            break;
        }

        event[2] = KeyDetector_GetEvent(&key[2]);
        switch (event[2])
        {
        case KEY_EVENT_CLICK:
            printf("Right Click detected\n");
            app_cdio_adjust_volume(0.05f);
            break;
        case KEY_EVENT_DOUBLE:
            app_cdio_set_next();
            printf("Right Double click detected\n");
            break;
        case KEY_EVENT_LONG_PRESS:
            printf("Right Long press detected\n");
            break;
        default:
            break;
        }

        if(event[0] != KEY_EVENT_NONE || event[1] != KEY_EVENT_NONE || event[2] != KEY_EVENT_NONE)
        {
            //beep_play_button(1);
            led_write(led_handle, 1);
            usleep(200000);
            led_write(led_handle, 0);
            led_set_trigger(led_handle, "rc-feedback");
            //beep_play_button(0);
        }

        sleep_ms(20);
    }

    return NULL;
}

void *button_read_entry(void *arg)
{
    struct input_event ev;
    int adc_button_device;

    adc_button_device = open(ADC_BUTTON_DEVICE, O_RDONLY);
    if (adc_button_device < 0)
    {
        perror("Failed to open adc_button");
        return NULL;
    }

    while (1)
    {
        ssize_t n = read(adc_button_device, &ev, sizeof(ev));
        if (n == (ssize_t)-1)
        {
            printf("Failed to read event\n");
            return NULL;
        }
        else if (n != sizeof(ev))
        {
            printf("Unexpected read size\n");
            return NULL;
        }

        if (ev.type == EV_KEY)
        {
            // printf("(code %d) state %d\n", ev.code, ev.value);

            if (ev.code == 132 && ev.value == 1) // left
            {
                key[0].keyRaw = true;
            }
            else if (ev.code == 133 && ev.value == 1) // middle
            {
                key[1].keyRaw = true;
            }
            else if (ev.code == 134 && ev.value == 1) // right
            {
                key[2].keyRaw = true;
            }

            if (ev.code == 132 && ev.value == 0) // left
            {
                key[0].keyRaw = false;
            }
            else if (ev.code == 133 && ev.value == 0) // middle
            {
                key[1].keyRaw = false;
            }
            else if (ev.code == 134 && ev.value == 0) // right
            {
                key[2].keyRaw = false;
            }
        }
    }

    close(adc_button_device);
    return NULL;
}

void *button_ir_entry(void *arg)
{
    const char *path = "/sys/class/rc/rc0/protocols";
    int fd = open(path, O_WRONLY);
    if (fd < 0)
    {
        perror("set_ir_protocol: open");
    }
    else
    {
        const char *proto = "nec"; // 设置为 NEC 编码格式来解码
        ssize_t wn = write(fd, proto, strlen(proto));
        if (wn < 0)
        {
            perror("set_ir_protocol: write");
        }
        else
        {
            // 类似于“echo”
            write(fd, "\n", 1);
        }
        close(fd);
    }

    struct input_event ev;
    int ir_fd = open(IR_BUTTON_DEVICE, O_RDONLY);
    if (ir_fd < 0)
    {
        perror("Failed to open ir device");
        return NULL;
    }

    struct pollfd pfd;
    pfd.fd = ir_fd;
    pfd.events = POLLIN;

    unsigned int last_code = 0;
    uint32_t last_time = 0;
    char digits[32];
    int dlen = 0;
    uint32_t last_digit_time = 0;
    memset(digits, 0, sizeof(digits));
    printf("IR button thread started\n");

    while (1)
    {
        int pret = poll(&pfd, 1, 200); // 200ms超时时间
        uint32_t now = GetSystemTime();

        if (dlen > 0 && (now - last_digit_time) > IR_DIGIT_TIMEOUT_MS)
        {
            printf("IR digit timeout, cleared\n");
            dlen = 0;
            digits[0] = '\0';
        }

        if (pret <= 0)  continue;

        if (pfd.revents & POLLIN)
        {
            ssize_t n = read(ir_fd, &ev, sizeof(ev));
            // printf("IR event type: %d  value: %x\n", ev.type, ev.value);
            if (n != sizeof(ev))
                continue;

            if (ev.type == EV_KEY && ev.code > 0 && ev.value == 1)
            {
                unsigned int code = ev.code;
                if (code == last_code && (now - last_time) < IR_DEBOUNCE_MS)
                {
                    continue; // 类似防抖效果，防止多次触发
                }
                last_code = code;
                last_time = now;
                switch (code)
                {
                case KEY_PAUSE: // pause
                    app_cdio_toggle_stop();
                    printf("IR: pause\n");
                    break;
                case KEY_EJECTCD: // eject
                    app_cdio_set_eject();
                    printf("IR: eject\n");
                    break;
                case KEY_PREVIOUS: // prev
                    app_cdio_set_prev();
                    printf("IR: prev\n");
                    break;
                case KEY_NEXT: // next
                    app_cdio_set_next();
                    printf("IR: next\n");
                    break;
                case KEY_VOLUMEDOWN: // vol-
                    app_cdio_adjust_volume(-0.05f);
                    printf("IR: vol-\n");
                    break;
                case KEY_VOLUMEUP: // vol+
                    app_cdio_adjust_volume(0.05f);
                    printf("IR: vol+\n");
                    break;
                case KEY_NUMERIC_0: // 1
                case KEY_NUMERIC_1: // 2
                case KEY_NUMERIC_2: // 3
                case KEY_NUMERIC_3: // 4
                case KEY_NUMERIC_4: // 5
                case KEY_NUMERIC_5: // 6
                case KEY_NUMERIC_6: // 7
                case KEY_NUMERIC_7: // 8
                case KEY_NUMERIC_8: // 9
                case KEY_NUMERIC_9: // 0
                {
                    int digit = -1;
                    if (code == KEY_NUMERIC_0)
                        digit = 0;
                    else if (code == KEY_NUMERIC_1)
                        digit = 1;
                    else if (code == KEY_NUMERIC_2)
                        digit = 2;
                    else if (code == KEY_NUMERIC_3)
                        digit = 3;
                    else if (code == KEY_NUMERIC_4)
                        digit = 4;
                    else if (code == KEY_NUMERIC_5)
                        digit = 5;
                    else if (code == KEY_NUMERIC_6)
                        digit = 6;
                    else if (code == KEY_NUMERIC_7)
                        digit = 7;
                    else if (code == KEY_NUMERIC_8)
                        digit = 8;
                    else if (code == KEY_NUMERIC_9)
                        digit = 9;

                    if (digit >= 0)
                    {
                        if (dlen < (int)sizeof(digits) - 1)
                        {
                            digits[dlen++] = '0' + digit;
                            digits[dlen] = '\0';
                            last_digit_time = now;
                            printf("IR digit appended: %s\n", digits);
                        }
                    }
                }
                break;
                case KEY_ENTER: // confirm
                    if (dlen > 0)
                    {
                        uint16_t track_num = (uint16_t)atoi(digits);
                        printf("IR confirm: play track %d\n", track_num);
                        app_cdio_set_manual_track(track_num);
                        dlen = 0;
                        digits[0] = '\0';
                    }
                    break;
                default:
                    break;
                }
            } 
        }
    }

    close(ir_fd);
    return NULL;
}
