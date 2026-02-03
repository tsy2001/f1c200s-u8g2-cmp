CC=arm-buildroot-linux-uclibcgnueabi-gcc
SYSROOT=/home/tsy/F1C200S/flash/buildroot-2025.02.9/output/host/arm-buildroot-linux-uclibcgnueabi/sysroot

TARGET=u8g2_hw_i2c_dvd

ROOT := $(abspath ..)

OBJDIR := $(ROOT)/output/obj
OUTDIR := $(ROOT)/output/bin

CPPFLAGS := \
  -I$(CURDIR) \
  -I$(ROOT) \
  -I$(ROOT)/drivers/src \
  -I$(ROOT)/hardware/BEEP \
  -I$(ROOT)/hardware/I2C \
  -I$(ROOT)/hardware/MPU6050 \
  -I$(ROOT)/dr_libs \
  -D__ARM_LINUX__

CFLAGS  := --sysroot=$(SYSROOT) -std=c99 -Wall -MMD -MP -O2 -g0 -pipe -flto=auto \
  -march=armv5te -mtune=arm926ej-s -marm \
  -mfloat-abi=soft -msoft-float \
  -fomit-frame-pointer \
  -fno-strict-aliasing \
  -ffunction-sections -fdata-sections \
  -D_POSIX_C_SOURCE=200809L
LDFLAGS := --sysroot=$(SYSROOT) -flto=auto -Wl,-O1 -Wl,--as-needed -Wl,--gc-sections
LDLIBS  := -lrt -lasound -lcdio -lpthread -lu8g2 -lu8g2font -lm

LOCAL_SRCS := \
  u8g2_hw_i2c.c \
  dvdplayer.c \
  dr_libs_impl.c \
  app_button.c \
  app_u8g2.c \
  app_cdplayer.c

ROOT_SRCS := \
  drivers/src/led.c \
  drivers/src/pwm.c \
  hardware/BEEP/beep.c \
  dsd2pcm/dsd2pcm.c

SRCS := $(LOCAL_SRCS) $(ROOT_SRCS)

OBJS := $(patsubst %.c,$(OBJDIR)/%.o,$(SRCS))
DEPS := $(OBJS:.o=.d)

VPATH := $(CURDIR) $(ROOT)

.PHONY: all clean directories

all: directories $(TARGET)

directories:
	@mkdir -p $(OBJDIR)
	@mkdir -p $(OUTDIR)

$(TARGET): $(OBJS)
	@echo Generating $(TARGET) ...
	@$(CC) -o $(OUTDIR)/$@ $^ $(LDFLAGS) $(LDLIBS)

$(OBJDIR)/%.o: %.c
	@mkdir -p $(dir $@)
	$(CC) $(CPPFLAGS) $(CFLAGS) -c $< -o $@

-include $(DEPS)

clean:
	@echo RM -rf $(OBJDIR)/
	@rm -rf $(OBJDIR)
	@echo RM -rf $(OUTDIR)/
	@rm -rf $(OUTDIR)
