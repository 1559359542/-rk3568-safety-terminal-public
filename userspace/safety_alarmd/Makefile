CROSS_COMPILE ?= /opt/atk-dlrk356x-toolchain/usr/bin/aarch64-buildroot-linux-gnu-
CC := $(CROSS_COMPILE)gcc

CFLAGS ?= -std=c11 -Wall -Wextra -Werror -O2

DAEMON_TARGET := safety_alarmd
DAEMON_SOURCE := safety_alarmd.c
CONTROL_TARGET := safety_alarmctl
CONTROL_SOURCE := safety_alarmctl.c
BRIDGE_TARGET := ai_alarm_bridge
BRIDGE_SOURCE := ai_alarm_bridge.c
UAPI_HEADER := ../../drivers/safety_event/safety_event_uapi.h
TARGETS := $(DAEMON_TARGET) $(CONTROL_TARGET) $(BRIDGE_TARGET)

.PHONY: all clean

all: $(TARGETS)

$(DAEMON_TARGET): $(DAEMON_SOURCE) $(UAPI_HEADER)
	$(CC) $(CFLAGS) -o $@ $(DAEMON_SOURCE)

$(CONTROL_TARGET): $(CONTROL_SOURCE)
	$(CC) $(CFLAGS) -o $@ $(CONTROL_SOURCE)

$(BRIDGE_TARGET): $(BRIDGE_SOURCE)
	$(CC) $(CFLAGS) -o $@ $(BRIDGE_SOURCE)

clean:
	rm -f $(TARGETS)
