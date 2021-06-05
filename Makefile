include Kbuild

ifeq ($(KERNELRELEASE),)

KDIR := /lib/modules/$(shell uname -r)/build
DKMS_PACKAGE := $(shell source $(CURDIR)/dkms.conf && echo $${PACKAGE_NAME})
DKMS_VERSION := $(shell source $(CURDIR)/dkms.conf && echo $${PACKAGE_VERSION})
DKMS_PV := $(DKMS_PACKAGE)/$(DKMS_VERSION)

all: modules compile_commands.json

modules clean modules_install:
	$(MAKE) -C $(KDIR) M=$(CURDIR) $@

install: modules_install

.PHONY: all modules clean install modules_install

.SUFFIXES:

%:
	$(MAKE) -C $(KDIR) M=$(CURDIR) $@

OBJ_FILE := $(obj-m)
SRC_FILE := $(OBJ_FILE:.o=.c)
CMD_FILE := .$(OBJ_FILE).cmd
MODNAME := $(OBJ_FILE:.o=)

$(OBJ_FILE) $(MODNAME).ko: $(SRC_FILE) Kbuild

reload:
	-/sbin/rmmod $(MODNAME)
	/sbin/insmod $(MODNAME).ko

.PHONY: reload

dkms-add:
	/usr/sbin/dkms add $(CURDIR)

dkms-remove:
	/usr/sbin/dkms remove $(DKMS_PV) --all

dkms-build dkms-install dkms-uninstall: dkms-%:
	/usr/sbin/dkms $* $(DKMS_PV)

.PHONY: dkms-add dkms-remove dkms-build dkms-install dkms-uninstall

modprobe:
	modprobe $(MODNAME)

modprobe-remove:
	modprobe -r $(MODNAME)

.PHONY: modprobe modprobe-remove

format:
	clang-format -i $(SRC_FILE)

.PHONY: format

compile_commands.json: $(OBJ_FILE)
	python3 .vscode/generate_compdb.py -O $(KDIR) $(CURDIR)

endif
