obj-m += mtdx_core.o
obj-m += rand_peb_alloc.o long_map.o
obj-m += mtdx_block.o ftl_simple.o ms_block.o # test_mtdx_block.o

mtdx_core-y := mtdx_bus.o mtdx_data.o

KERNELDIR ?= /lib/modules/$(shell uname -r)/build
#KERNELDIR ?= ~/linux-2.6
#KERNELDIR ?= /usr/src/linux-2.6.23
PWD       := $(shell pwd)
EXTRA_CFLAGS = -g -fno-inline -DDEBUG -DCONFIG_DEBUG_SPINLOCK

all:
	echo $(PWD)
	$(MAKE) -C $(KERNELDIR) M=$(PWD)

clean:
	rm -rf *.o *.o.d *.ko.d .depend .*.cmd *.ko *.mod.c .tmp_versions

