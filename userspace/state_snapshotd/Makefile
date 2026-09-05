CROSS_COMPILE ?= /opt/atk-dlrk356x-toolchain/usr/bin/aarch64-buildroot-linux-gnu-
CC := $(CROSS_COMPILE)gcc
CFLAGS ?= -std=c11 -Wall -Wextra -Werror -O2
TARGET := state_snapshotd

all: $(TARGET)

$(TARGET): state_snapshotd.c
	$(CC) $(CFLAGS) -o $@ $<

clean:
	rm -f $(TARGET)
