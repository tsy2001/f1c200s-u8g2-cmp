CC=arm-buildroot-linux-musleabi-gcc
SYSROOT=/home/tsy/F1C200S/flash/buildroot-2025.02.9/output/host/arm-buildroot-linux-musleabi/sysroot

TARGET=u8g2_hw_i2c_dvd
IDIR= -I ./ -I ../drivers/src -I ../u8g2/csrc -I ../u8g2/sys/arm-linux/port -I ../hardware/BEEP -I ../hardware/I2C -I ../hardware/MPU6050 -I ../dr_libs
SRCDIR=../u8g2/csrc
OBJDIR=../output/obj
OUTDIR=../output/bin
LDIR= --sysroot=$(SYSROOT) -lrt -lasound -lcdio -lpthread
LIBS=  -lm

CFLAGS= $(IDIR) --sysroot=$(SYSROOT) -std=c99 -lrt -W -Wall -D__ARM_LINUX__

OBJ+=u8g2_hw_i2c.o\
	dvdplayer.o\
	dr_libs_impl.o\
	app_button.o\
	app_u8g2.o\
	app_cdplayer.o\
	../u8g2/sys/arm-linux/port/u8g2port.o\
	../drivers/src/gpio.o\
	../drivers/src/gpio_sysfs.o\
	../drivers/src/led.o\
	../drivers/src/spi.o\
	../drivers/src/i2c.o\
	../drivers/src/pwm.o\
	../hardware/BEEP/beep.o\

OBJ+=$(patsubst $(SRCDIR)/%.c,$(OBJDIR)/%.o, $(wildcard $(SRCDIR)/*.c))

all: directories $(TARGET) 

directories:
	@mkdir -p $(OBJDIR)
	@mkdir -p $(OUTDIR)

$(TARGET):$(OBJ)
	@echo Generating $(TARGET) ...
	@$(CC) -o $(OUTDIR)/$@ $(OBJ) $(LDIR) $(LIBS)

$(OBJDIR)/%.o: $(SRCDIR)/%.c
	$(CC) -c -o $@ $< $(CFLAGS) $(LDIR) $(LIBS)

clean:
	@echo RM -rf $(OBJDIR)/
	@rm -rf $(OBJ)
	@rm -rf $(OBJDIR)
	
	@echo RM -rf $(OUTDIR)/
	@rm -rf $(OUTDIR)

