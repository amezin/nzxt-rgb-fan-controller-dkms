include Kbuild

ifeq ($(KERNELRELEASE),)

SHELL := /bin/bash

KDIR := /lib/modules/$(shell uname -r)/build
DKMS_PACKAGE := $(shell source $(CURDIR)/dkms.conf && echo $${PACKAGE_NAME})
DKMS_VERSION := $(shell source $(CURDIR)/dkms.conf && echo $${PACKAGE_VERSION})

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

dkms-add: dkms-%:
	/usr/sbin/dkms $* $(CURDIR)

dkms-build dkms-install dkms-uninstall: dkms-%:
	/usr/sbin/dkms $* $(DKMS_PACKAGE)/$(DKMS_VERSION)

dkms-remove: dkms-%:
	/usr/sbin/dkms $* $(DKMS_PACKAGE)/$(DKMS_VERSION) --all

.PHONY: dkms-add dkms-remove dkms-build dkms-install dkms-uninstall

modprobe:
	modprobe $(MODNAME)

modprobe-remove:
	modprobe -r $(MODNAME)

.PHONY: modprobe modprobe-remove

format: .clang-format
	clang-format -i $(SRC_FILE)

.PHONY: format

compile_commands.json: $(OBJ_FILE)
	python3 .vscode/generate_compdb.py -O $(KDIR) $(CURDIR)

.clang-format .gitattributes:
	curl -o $@ https://git.kernel.org/pub/scm/linux/kernel/git/stable/linux.git/plain/$@

.gitignore: custom.gitignore
	curl -o $@ https://git.kernel.org/pub/scm/linux/kernel/git/stable/linux.git/plain/$@
	cat $< >> $@

checkpatch:
	$(KDIR)/scripts/checkpatch.pl --no-tree -f $(SRC_FILE)

checkpatch-fix:
	$(KDIR)/scripts/checkpatch.pl --no-tree --fix-inplace -f $(SRC_FILE)

.PHONY: checkpatch checkpatch-fix

endif
