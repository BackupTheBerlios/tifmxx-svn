obj-m += tifm_core.o tifm_7xx1.o tifm_sd.o
obj-m += memstick.o tifm_ms.o ms_block.o


KERNELDIR ?= /lib/modules/$(shell uname -r)/build
#KERNELDIR ?= ~/linux-2.6
PWD       := $(shell pwd)
MODULE_INSTALLDIR = /lib/modules/$(shell uname -r)
EXTRA_CFLAGS = -DDEBUG

all:
	echo $(PWD)
	$(MAKE) -C $(KERNELDIR) M=$(PWD)

clean:
	rm -rf *.o *.o.d *.ko.d .depend .*.cmd *.ko *.mod.c .tmp_versions

install:
	mkdir -p $(MODULE_INSTALLDIR)
	rm -f $(MODULE_INSTALLDIR)/kernel/drivers/misc/tifm_core.ko
	rm -f $(MODULE_INSTALLDIR)/kernel/drivers/misc/tifm_7xx1.ko
	rm -f $(MODULE_INSTALLDIR)/kernel/drivers/mmc/tifm_sd.ko

	install -c -m 644 tifm_core.ko tifm_7xx1.ko $(MODULE_INSTALLDIR)/kernel/drivers/misc/
	install -c -m 644 tifm_sd.ko $(MODULE_INSTALLDIR)/kernel/drivers/mmc/
	/sbin/depmod -ae
