CROSS_COMPILE ?= /opt/atk-dlrk356x-toolchain/usr/bin/aarch64-buildroot-linux-gnu-
CC := $(CROSS_COMPILE)gcc

SYSROOT ?= /home/lyy/rk3568/SDK/buildroot/output/rockchip_rk3568/host/aarch64-buildroot-linux-gnu/sysroot

CPPFLAGS ?= --sysroot=$(SYSROOT)
CFLAGS ?= -std=c11 -Wall -Wextra -Werror -O2
LDFLAGS ?= --sysroot=$(SYSROOT)
LDLIBS ?= -lpaho-mqtt3c -lsqlite3 -lpthread -ldl

TARGET := mqtt_reporterd
SOURCES := mqtt_reporterd.c jsmn.c
HEADERS := jsmn.h

.PHONY: all clean

all: $(TARGET)

$(TARGET): $(SOURCES) $(HEADERS)
	$(CC) $(CPPFLAGS) $(CFLAGS) $(LDFLAGS) -o $@ $(SOURCES) $(LDLIBS)

clean:
	rm -f $(TARGET)
