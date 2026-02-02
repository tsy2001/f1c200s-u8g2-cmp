#ifndef _APP_BUTTON_H_
#define _APP_BUTTON_H_

#include <stdio.h>
#include <stdint.h>
#include <stdbool.h>

// 按键状态定义
typedef enum {
    KEY_STATE_IDLE,      // 空闲状态
    KEY_STATE_PRESS,     // 按下状态
    KEY_STATE_RELEASE,   // 释放状态
    KEY_STATE_WAIT_DOUBLE, // 等待第二次按下
    KEY_STATE_LONG_PRESS // 长按状态
} KeyState;

// 按键事件定义
typedef enum {
    KEY_EVENT_NONE,      // 无事件
    KEY_EVENT_CLICK,     // 单击
    KEY_EVENT_DOUBLE,    // 双击
    KEY_EVENT_LONG_PRESS // 长按
} KeyEvent;

// 按键检测结构体
typedef struct {
    KeyState state;          // 当前状态
    unsigned long pressTime;      // 按下时间
    unsigned long releaseTime;    // 释放时间
    bool keyRaw;
    bool keyPressed;         // 当前按键是否按下
    bool keyPressedLast;     // 上一次按键状态
    KeyEvent event;          // 检测到的事件
} KeyDetector;


#define DEBOUNCE_TIME       10      // 消抖时间(ms)
#define CLICK_TIME          240     // 单击最大时间(ms)
#define DOUBLE_CLICK_TIME   180     // 双击间隔时间(ms)
#define LONG_PRESS_TIME     1000    // 长按时间(ms)

#define IR_DEBOUNCE_MS      200     // IR消抖时间(ms)
#define IR_DIGIT_TIMEOUT_MS 5000

void app_button_start(void);
void app_button_wait(void);
void *button_read_entry(void *);
void *button_handl_entry(void *);
void *button_ir_entry(void *arg);

#endif
