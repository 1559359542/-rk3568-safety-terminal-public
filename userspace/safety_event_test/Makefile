# ARM64 Buildroot 交叉工具链前缀
CROSS_COMPILE ?= /opt/atk-dlrk356x-toolchain/usr/bin/aarch64-buildroot-linux-gnu-
CC := $(CROSS_COMPILE)gcc

# 将常见警告视为错误，尽早发现用户态接口问题
CFLAGS ?= -std=c11 -Wall -Wextra -Werror -O2

TARGET := safety_event_uapi_test
SOURCE := safety_event_uapi_test.c
UAPI_HEADER := ../../drivers/safety_event/safety_event_uapi.h

.PHONY: all clean

all: $(TARGET)

$(TARGET): $(SOURCE) $(UAPI_HEADER)
	$(CC) $(CFLAGS) -o $@ $(SOURCE)

clean:
	rm -f $(TARGET)
