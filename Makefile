ifneq ($(KERNELRELEASE),)

include Kbuild

else

KDIR := /lib/modules/$(shell uname -r)/build
DKMS_VERSION := $(shell source $(CURDIR)/dkms.conf && echo $${PACKAGE_VERSION})

modules clean modules_install:
	$(MAKE) -C $(KDIR) M=$(CURDIR) $@

install: modules_install

.PHONY: modules clean install modules_install

.SUFFIXES:

%:
	$(MAKE) -C $(KDIR) M=$(CURDIR) $@

nzxt_grid.ko nzxt_grid.o: nzxt_grid.c Kbuild

reload:
	-/sbin/rmmod nzxt_grid
	/sbin/insmod nzxt_grid.ko

.PHONY: reload

dkms-add:
	/usr/sbin/dkms add $(CURDIR)

dkms-remove:
	/usr/sbin/dkms remove nzxt_grid/$(DKMS_VERSION) --all

dkms-build dkms-install dkms-uninstall: dkms-%:
	/usr/sbin/dkms $* nzxt_grid/$(DKMS_VERSION)

.PHONY: dkms-add dkms-remove dkms-build dkms-install dkms-uninstall

modprobe:
	modprobe nzxt_grid

modprobe-remove:
	modprobe -r nzxt_grid

.PHONY: modprobe modprobe-remove

format:
	clang-format -i nzxt_grid.c

.PHONY: format

compile_commands.json: nzxt_grid.o
	python3 .vscode/generate_compdb.py -O $(KDIR) $(CURDIR)

endif
