obj-m	    += tifmxx.o
tifmxx-objs += tifmxx_hw.o tifmxx_scsi.o tifmxx_mmcsd.o

KERNELDIR ?= /lib/modules/$(shell uname -r)/build
PWD       := $(shell pwd)

all:
	$(MAKE) -C $(KERNELDIR) M=$(PWD)

clean:
	rm -rf *.o *.o.d .depend .*.cmd *.ko *.mod.c .tmp_versions

