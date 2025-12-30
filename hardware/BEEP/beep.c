#include <stdio.h>
#include <fcntl.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <sys/time.h>
#include <unistd.h>
#include <string.h>
#include <sys/ioctl.h>
#include <linux/ioctl.h>

#include "pwm.h"
#include "beep.h"
#include "u8g2port.h"

static pwm_t *pwm_bandles = NULL;

uint16_t tone[] = {247,262,294,330,349,392,440,494,523,587,659,698,784,880,988,1046,1000};

uint16_t toneshumabaolong[] = {247, 262, 294, 330, 349, 392, 440, 294, 789, 840, 991, 1060, 1112, 1000};

uint8_t music[] = {
    5,10,10,5,5,9,9,16,8,8,8,9,10,5,5,16,//想去远方的山川，想去海边看海鸥
    6,8,8,6,5,10,10,16,9,8,8,6,9,16,//不管风雨有多少，有你就足够
    5,10,10,5,5,9,9,16,8,8,8,6,5,10,10,16,//喜欢看你的嘴角，喜欢看你的眉梢
    6,11,11,6,5,10,10,16,9,8,8,6,8,16,//白云挂在那蓝天，像你的微笑
    5,12,5,5,12,5,9,16,8,6,8,8,8,10,12,16,//你笑起来真好看，像春天的花一样！
    8,6,8,8,8,13,12,10,9,8,6,8,8,10,9,16,//把所有的烦恼，所有的忧愁，统统都吹散
    5,12,5,5,12,5,9,16,8,6,8,8,13,12,16,//你笑起来真好看，像夏天的阳光
    8,8,8,13,12,10,9,8,6,8,8,9,8,16,//整个世界全部的时光，美得像画卷。
};

uint8_t music_time[] = {
    4,4,4,4,4,4,4,4,4,4,4,4,4,4,4,4,//想去远方的山川，想去海边看海鸥
    4,4,4,4,4,4,4,4,4,4,4,4,8,4,//不管风雨有多少，有你就足够
    4,4,4,4,4,4,4,4,4,4,4,4,4,4,4,4,//喜欢看你的嘴角，喜欢看你的眉梢
    4,4,4,4,4,4,4,4,4,4,4,4,8,4,//白云挂在那蓝天，像你的微笑
    4,4,2,2,4,4,4,4,4,4,2,2,4,4,8,4,//你笑起来真好看，像春天的花一样！
    4,4,2,2,4,4,4,4,4,4,4,4,4,4,8,4,//把所有的烦恼，所有的忧愁，统统都吹散
    4,4,2,2,4,4,4,4,4,4,4,4,4,8,4,//你笑起来真好看，像夏天的阳光
    4,4,4,4,4,4,4,4,4,4,4,4,8,4,//整个世界全部的时光，美得像画卷。
};

void beep_play_button(uint8_t press)
{
    if (press)
    {   pwm_set_duty_cycle(pwm_bandles, 0.5);
        pwm_set_frequency(pwm_bandles, 2000);
        //pwm_set_enabled(pwm_bandles, true);
    }
    else
    {
        pwm_set_duty_cycle(pwm_bandles, 0);
        //pwm_set_enabled(pwm_bandles, false);
    }
}

void beep_play_notify(void)
{
    uint8_t music[] = {10, 13, 8, 13, 10, 13, 8, 13, 11, 13, 9, 13, 11, 13, 9, 13};
    uint8_t time[] = {1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 2};

    uint32_t yanshi;
    uint16_t i, e;
    yanshi = 14;

    pwm_set_enabled(pwm_bandles, true);
    pwm_set_duty_cycle(pwm_bandles, 0.5);

    for (i = 0; i < sizeof(music) / sizeof(music[0]); i++)
    {
        for (e = 0; e < ((uint16_t)time[i]) * toneshumabaolong[music[i]] / yanshi; e++)
        {
            uint32_t slptime = 1000000 / (toneshumabaolong[music[i]]);
            if(toneshumabaolong[music[i]] != 1000)
                pwm_set_frequency(pwm_bandles, toneshumabaolong[music[i]]);
            sleep_us(slptime);
        }
    }

    pwm_set_duty_cycle(pwm_bandles, 0);
    pwm_set_enabled(pwm_bandles, false);
}

void beep_play_music(void)
{
    uint32_t yanshi = 10;
    uint16_t i, e;

    pwm_set_enabled(pwm_bandles, true);
    pwm_set_duty_cycle(pwm_bandles, 0.5);

    for (i = 0; i < sizeof(music) / sizeof(music[0]); i++)
    {
        for (e = 0; e < ((uint16_t)music_time[i]) * tone[music[i]] / yanshi; e++)
        {
            // Sound((u32)tone[music[i]]);
            uint32_t slptime = 1000000 / (tone[music[i]]);
            if(tone[music[i]] != 1000)
                pwm_set_frequency(pwm_bandles, tone[music[i]]);
            sleep_us(slptime);
        }
    }

    pwm_set_duty_cycle(pwm_bandles, 0);
    pwm_set_enabled(pwm_bandles, false);
}

void beep_deinit(void)
{
    pwm_set_enabled(pwm_bandles, false);
    pwm_disable(pwm_bandles);
    pwm_close(pwm_bandles);
    pwm_free(pwm_bandles);
}

int beep_init(void)
{
    int err = -1;
    pwm_bandles = pwm_new();
    if(pwm_bandles == NULL)
    {
        printf("pwm handles create fail \n");
        return -1;
    }

    err = pwm_open(pwm_bandles, 0, 1);
    if(err != 0)
    {
        printf("pwm devide regestor error \n");
        return -1;
    }

    err = pwm_set_polarity(pwm_bandles, PWM_POLARITY_NORMAL);
    if(err < 0)
    {
        printf("failed to change pwm1 polarity to normal\n");
        return -1;
    }

    err = pwm_set_frequency(pwm_bandles, 2000);
    if(err < 0)
    {
        printf("failed to set pwm frequency\n");
        return -1;
    }

    err = pwm_set_duty_cycle(pwm_bandles, 0);
    if(err < 0)
    {
        printf("failed to set pwm dutycycle\n");
        return -1;
    }

    err = pwm_set_enabled(pwm_bandles, true);
    if(err < 0)
    {
        printf("failed to enable pwm\n");
        return -1;
    }
    return 0;
}