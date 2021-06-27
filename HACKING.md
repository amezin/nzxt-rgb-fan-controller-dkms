Makefile
========

`Makefile` provides a convenient wrapper around KBuild.

Default target - `all` - builds the module for the kernel at `KDIR`:

    $ make KDIR=/lib/modules/5.10.46-1-lts/build

When `KDIR` is not specified explicitly, currently booted kernel (`uname -r`)
will be targeted.

Other targets:

- `install`: installs the module.

- `insmod`: loads the built module, without installing.

- `rmmod`: unloads the module.

- `reload`: unloads the module (ignoring possible errors), then loads it (as in
`insmod`).

- `checkpatch`: checks module source code using `scripts/checkpatch.pl` from the
kernel.

- `checkpatch-fix`: checks module source code using `scripts/checkpatch.pl` from
the kernel and fixes issues, if possible (in place).

- `format`: reformats module source code using `clang-format` (in place).
Caution: `clang-format` sometimes conflicts with `checkpatch`

When `.vscode` submodule is checked out, `make all` also generates
`compile_commands.json`.

compile_commands.json
=====================

`compile_commands.json` allows using clang-based language servers to assist in
code editing. In particular, you could edit the code in:

- Visual Studio Code with C/C++ extension: `code .`

- Sublime Text with [LSP package](https://packagecontrol.io/packages/LSP) and
`clangd`: `subl nzxt-rgb-fan-controller.sublime-project`

Vagrant
=======

There is also a `Vagrantfile` (plus short ansible playbooks in `ansible/`).
Both Vagrant and Ansible need to be installed on the host.

`vagrant up` will bring up a Fedora VM, will install a debug kernel, and reboot
into it, if necessary. Supported USB devices will be passed through into the VM,
so the module could be tested inside of the VM.

VirtualBox and libvirt providers are supported, libvirt is recommended
(`vagrant up --provider libvirt`) if available.
