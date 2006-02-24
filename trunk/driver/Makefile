obj-m	    += tifm_core.o

KERNELDIR ?= /lib/modules/$(shell uname -r)/build
PWD       := $(shell pwd)

all:
	$(MAKE) -C $(KERNELDIR) M=$(PWD)

clean:
	rm -rf *.o .*.o.d .depend .*.cmd *.ko *.mod.c .tmp_versions

