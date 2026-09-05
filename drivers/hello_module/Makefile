obj-m += hello.o

KDIR ?= /home/lyy/rk3568/SDK/kernel
ARCH ?= arm64
CROSS_COMPILE ?= /opt/atk-dlrk356x-toolchain/usr/bin/aarch64-buildroot-linux-gnu-

all:
	$(MAKE) -C $(KDIR) M=$(CURDIR) ARCH=$(ARCH) CROSS_COMPILE=$(CROSS_COMPILE) modules

clean:
	$(MAKE) -C $(KDIR) M=$(CURDIR) clean