CROSS_COMPILE ?= /opt/atk-dlrk356x-toolchain/usr/bin/aarch64-buildroot-linux-gnu-
CC := $(CROSS_COMPILE)gcc

CFLAGS ?= -std=c11 -Wall -Wextra -Werror -O2
LDLIBS ?= -lsqlite3
TARGET := sqlite_eventd

.PHONY: all clean

all: $(TARGET)

$(TARGET): sqlite_eventd.c
	$(CC) $(CFLAGS) -o $@ $< $(LDLIBS)

clean:
	rm -f $(TARGET)
