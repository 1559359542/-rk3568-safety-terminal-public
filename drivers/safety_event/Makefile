obj-m += safety_event.o # 将 safety_event.c 构建为外部内核模块

KDIR ?= /home/lyy/rk3568/SDK/kernel # SDK 内核源码与构建目录
ARCH ?= arm64 # 目标板 CPU 架构，不使用 Ubuntu 主机的 x86 默认值
CROSS_COMPILE ?= /opt/atk-dlrk356x-toolchain/usr/bin/aarch64-buildroot-linux-gnu- # ARM64 交叉编译器前缀

all:
	$(MAKE) -C $(KDIR) M=$(CURDIR) ARCH=$(ARCH) CROSS_COMPILE=$(CROSS_COMPILE) modules # 由 SDK Kbuild 构建当前目录模块

clean:
	$(MAKE) -C $(KDIR) M=$(CURDIR) clean # 只清理当前项目驱动目录的构建产物
