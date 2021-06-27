include Kbuild

ifeq ($(KERNELRELEASE),)

KDIR := /lib/modules/$(shell uname -r)/build

all: modules

GEN_COMPILE_COMMANDS := .vscode/generate_compdb.py

ifneq ($(wildcard $(GEN_COMPILE_COMMANDS)),)
all: compile_commands.json
endif

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

insmod:
	/sbin/insmod $(MODNAME).ko

rmmod:
	/sbin/rmmod $(MODNAME)

reload:
	-/sbin/rmmod $(MODNAME)
	/sbin/insmod $(MODNAME).ko

.PHONY: insmod rmmod reload

format: .clang-format
	clang-format -i $(SRC_FILE)

.PHONY: format

compile_commands.json: $(OBJ_FILE) $(GEN_COMPILE_COMMANDS)
	python3 $(GEN_COMPILE_COMMANDS) -O $(KDIR) $(CURDIR)

upstream_config/%:
	curl -o $@ https://git.kernel.org/pub/scm/linux/kernel/git/stable/linux.git/plain/$*

.gitignore: upstream_config/.gitignore custom.gitignore
	cat $^ >$@

.clang-format: clang-format.sed upstream_config/.clang-format
	sed -E -f $^ >$@

checkpatch:
	$(KDIR)/scripts/checkpatch.pl --strict --no-tree -f $(SRC_FILE)

checkpatch-fix:
	$(KDIR)/scripts/checkpatch.pl --strict --no-tree --fix-inplace -f $(SRC_FILE)

.PHONY: checkpatch checkpatch-fix

endif
