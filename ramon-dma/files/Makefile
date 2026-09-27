# SPDX-License-Identifier: GPL-2.0
#
# Out-of-tree build of ramon_dma.ko. PetaLinux (module.bbclass) passes KERNEL_SRC.
# Objects are added here as each implementation step lands.

obj-m := ramon_dma.o
ramon_dma-objs := ramon_core.o ramon_of.o ramon_buf.o ramon_regwin.o ramon_axidma.o ramon_zdma.o ramon_spw.o ramon_spfi.o

KERNEL_SRC ?= /lib/modules/$(shell uname -r)/build
SRC := $(shell pwd)

all:
	$(MAKE) -C $(KERNEL_SRC) M=$(SRC)

modules_install:
	$(MAKE) -C $(KERNEL_SRC) M=$(SRC) modules_install

clean:
	rm -f *.o *.ko *.mod *.mod.c .*.cmd .*.d modules.order Module.symvers
	rm -rf .tmp_versions
