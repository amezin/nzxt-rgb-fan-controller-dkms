ifneq ($(KERNELRELEASE),)

include Kbuild

else

MODNAME := nzxt_grid

KDIR := /lib/modules/$(shell uname -r)/build
DKMS_VERSION := $(shell source $(CURDIR)/dkms.conf && echo $${PACKAGE_VERSION})

all: modules compile_commands.json

modules clean modules_install:
	$(MAKE) -C $(KDIR) M=$(CURDIR) $@

install: modules_install

.PHONY: all modules clean install modules_install

.SUFFIXES:

%:
	$(MAKE) -C $(KDIR) M=$(CURDIR) $@

SRC_FILE := $(MODNAME).c
OBJ_FILE := $(SRC_FILE:.c=.o)
CMD_FILE := .$(OBJ_FILE).cmd

$(OBJ_FILE) $(MODNAME).ko: $(SRC_FILE) Kbuild

reload:
	-/sbin/rmmod $(MODNAME)
	/sbin/insmod $(MODNAME).ko

.PHONY: reload

dkms-add:
	/usr/sbin/dkms add $(CURDIR)

dkms-remove:
	/usr/sbin/dkms remove $(MODNAME)/$(DKMS_VERSION) --all

dkms-build dkms-install dkms-uninstall: dkms-%:
	/usr/sbin/dkms $* $(MODNAME)/$(DKMS_VERSION)

.PHONY: dkms-add dkms-remove dkms-build dkms-install dkms-uninstall

modprobe:
	modprobe $(MODNAME)

modprobe-remove:
	modprobe -r $(MODNAME)

.PHONY: modprobe modprobe-remove

format:
	clang-format -i $(MODNAME).c

.PHONY: format

compile_commands.json: $(OBJ_FILE)
	python3 .vscode/generate_compdb.py -O $(KDIR) $(CURDIR)

endif
