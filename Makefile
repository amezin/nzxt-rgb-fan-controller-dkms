include Kbuild

KERNELRELEASE := $(shell uname -r)
KDIR := /lib/modules/$(KERNELRELEASE)/build

OBJ_FILE := $(obj-m)
SRC_FILE := $(OBJ_FILE:.o=.c)
CMD_FILE := .$(OBJ_FILE).cmd
MODNAME := $(OBJ_FILE:.o=)

all: modules
install: modules_install

$(OBJ_FILE) $(MODNAME).ko: $(SRC_FILE) Kbuild

modules clean modules_install $(OBJ_FILE) $(MODNAME).ko:
	$(MAKE) -C $(KDIR) M=$(CURDIR) $@

.PHONY: all modules clean install modules_install

.SUFFIXES:

.NOTPARALLEL:

# Load/unload/reload

insmod:
	/sbin/insmod $(MODNAME).ko

rmmod:
	/sbin/rmmod $(MODNAME)

reload:
	-/sbin/rmmod $(MODNAME)
	/sbin/insmod $(MODNAME).ko

.PHONY: insmod rmmod reload

# Getting and modifying configs from upstream

upstream_config/%:
	curl -o $@ https://git.kernel.org/pub/scm/linux/kernel/git/stable/linux.git/plain/$*

.gitignore: upstream_config/.gitignore custom.gitignore
	cat $^ >$@

.clang-format: clang-format.sed upstream_config/.clang-format
	sed -E -f $^ >$@

# Format

format: .clang-format
	clang-format -i $(SRC_FILE)

.PHONY: format

# checkpatch

checkpatch:
	$(KDIR)/scripts/checkpatch.pl -f $(SRC_FILE)

checkpatch-fix:
	$(KDIR)/scripts/checkpatch.pl --fix-inplace -f $(SRC_FILE)

.PHONY: checkpatch checkpatch-fix

# Sync the code with kernel tree

.push-upstream .pull-upstream:
	mkdir -p $@

UPSTREAM_SRCFILE := $(KDIR)/drivers/hwmon/$(SRC_FILE)
UPSTREAM_README := $(KDIR)/Documentation/hwmon/$(MODNAME).rst

.push-upstream/$(SRC_FILE): $(SRC_FILE) to-upstream.diff | .push-upstream
	patch -p1 -f -o $@ $< to-upstream.diff

.pull-upstream/$(SRC_FILE): $(UPSTREAM_SRCFILE) to-upstream.diff | .pull-upstream
	patch -p1 -f -R -o $@ $< to-upstream.diff

push-upstream: .push-upstream/$(SRC_FILE) README.rst
	cp $< $(UPSTREAM_SRCFILE)
	cp README.rst $(UPSTREAM_README)

pull-upstream: .pull-upstream/$(SRC_FILE) $(UPSTREAM_README)
	cp $< $(SRC_FILE)
	cp $(UPSTREAM_README) README.rst

.PHONY: push-upstream pull-upstream

# compile_commands.json (requires .vscode submodule)

GEN_COMPILE_COMMANDS := .vscode/generate_compdb.py

compile_commands.json: $(OBJ_FILE) $(GEN_COMPILE_COMMANDS)
	python3 $(GEN_COMPILE_COMMANDS) -O $(KDIR) $(CURDIR)

ifneq ($(wildcard $(GEN_COMPILE_COMMANDS)),)
all: compile_commands.json
endif
