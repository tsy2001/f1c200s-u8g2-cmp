#include <u8g2port.h>
#include <fcntl.h>
#include <unistd.h>
#include <signal.h>
#include <pthread.h>
#include "beep.h"
#include "app_u8g2.h"
#include "app_button.h"
#include "app_cdplayer.h"
#include "u8g2.h"
#include "u8g2port.h"


int main(void)
{
	//beep_init(); // todo: 内核PWM驱动极性反了,零占空比输出确是高电平

	app_u8g2_start();
	app_cdplayer_start();
	app_button_start();

	app_u8g2_wait();
	app_cdplayer_wait();
	app_button_wait();

	pthread_exit(NULL);
	
	//beep_deinit();
	printf("Exit\n");

	return 0;
}
