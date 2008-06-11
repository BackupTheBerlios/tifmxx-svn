obj-m += rand_peb_alloc.o sg_bounce.o mtdx_core.o ms_block.o

KERNELDIR ?= /lib/modules/$(shell uname -r)/build
#KERNELDIR ?= ~/linux-2.6
#KERNELDIR ?= /usr/src/linux-2.6.23
PWD       := $(shell pwd)
EXTRA_CFLAGS = -fno-inline -DDEBUG

all:
	echo $(PWD)
	$(MAKE) -C $(KERNELDIR) M=$(PWD)

clean:
	rm -rf *.o *.o.d *.ko.d .depend .*.cmd *.ko *.mod.c .tmp_versions

