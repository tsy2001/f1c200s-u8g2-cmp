#include <stdio.h>
#include <fcntl.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <sys/time.h>
#include <unistd.h>
#include <string.h>
#include <sys/ioctl.h>
#include <linux/ioctl.h>

#include "beep.h"

#define DUTY              "duty"
#define PERIOD            "1000000"
#define DUTYCYCLE         "0"

static int fd_chip = 0, fd_period = 0, fd_duty = 0, fd_enable = 0, fd_polarity;


void set_beep(uint16_t frequency)
{
    int ret = 0;
	if(f == 0)
    {
		ret = write(fd_duty, "0", strlen("0"));
        if(ret < 0)
        {
            return -1;
        }
	}
    else
    { 
        //发出指定频率的声音

	}
}


void pwm_destory(void)
{
    close(fd_chip);
    close(fd_period);
    close(fd_duty);
    close(fd_enable);
    close(fd_polarity);
}

int pwm_setup(void)
{
    int ret;

    fd_enable = open("/sys/class/pwm/pwmchip0/pwm1/enable", O_RDWR);
    if(fd_enable < 0)
    {    
        fd_chip = open("/sys/class/pwm/pwmchip0/export", O_WRONLY);
        if(fd_chip < 0)
        {
            printf("open export error\n");
            return -1;
        }

        ret = write(fd_chip, "1", strlen("0"));
        if(ret < 0)
        {
            printf("creat pwm1 error\n");
            return -1;
        }
        else
            printf("export pwm1 done\n");
    }

    fd_period = open("/sys/class/pwm/pwmchip0/pwm1/period", O_RDWR);
    fd_duty = open("/sys/class/pwm/pwmchip0/pwm1/duty_cycle", O_RDWR);
    fd_enable = open("/sys/class/pwm/pwmchip0/pwm1/enable", O_RDWR);
    fd_polarity = open("/sys/class/pwm/pwmchip0/pwm1/polarity", O_RDWR);
    
    if((fd_period < 0)||(fd_duty < 0)||(fd_enable < 0)||(fd_polarity < 0))
    {
        printf("pwm1 ops open error\n");
        return -1;
    }

    ret = write(fd_polarity, "normal", strlen("normal"));
    if(ret < 0)
    {
        printf("failed to change pwm1 polarity to normal\n");
        return -1;
    }

    ret = write(fd_period, PERIOD, strlen(PERIOD));
    if(ret < 0)
    {
        printf("change period error\n");
        return -1;
    }
    else
        printf("change period ok\n");

    ret = write(fd_duty, DUTYCYCLE, strlen(DUTYCYCLE));
    if(ret < 0)
    {
        printf("change duty_cycle error\n");
        return -1;
    }else
        printf("change duty_cycle ok\n");

    ret = write(fd_enable, "1", strlen("1"));
    if(ret < 0)
    {
        printf("enable pwm0 error\n");
        return -1;
    }else
        printf("enable pwm0 ok\n");

    return 0;
}

void beep_write(char *val)
{
    int ret = write(fd_duty, val, strlen(val));
    if(ret < 0)
    {
        printf("change duty_cycle to %s error\n", val);
        return -1;
    }
}

