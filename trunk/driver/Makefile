obj-m += tifm_core.o tifm_7xx1.o tifm_sd.o tifm_ms.o
obj-m += memstick.o ms_block.o


KERNELDIR ?= /lib/modules/$(shell uname -r)/build
#KERNELDIR ?= ~/linux-2.6
PWD       := $(shell pwd)
MODULE_INSTALLDIR = /lib/modules/$(shell uname -r)/misc
EXTRA_CFLAGS = -DDEBUG

all:
	echo $(PWD)
	$(MAKE) -C $(KERNELDIR) M=$(PWD)

clean:
	rm -rf *.o *.o.d *.ko.d .depend .*.cmd *.ko *.mod.c .tmp_versions

install:
	mkdir -p $(MODULE_INSTALLDIR)
	rm -f $(MODULE_INSTALLDIR)/tifm_core.ko
	rm -f $(MODULE_INSTALLDIR)/tifm_sd.ko
	rm -f $(MODULE_INSTALLDIR)/tifm_7xx1.ko
	install -c -m 644 tifm_core.ko tifm_7xx1.ko tifm_sd.ko $(MODULE_INSTALLDIR)
	/sbin/depmod -ae
