# xinu-rpi4 — top-level wrapper Makefile.
#
# The real build rules live in compile/Makefile (all objects and the flat
# kernel images are emitted there).  This wrapper only exists so the usual
# targets can be run from the repository ROOT — previously `make` at the
# root failed with "No rule to make target ..." because the only Makefile
# was one level down in compile/.
#
# Usage (from the repo root):
#     make            # build all three variants (pi4 + pi5 + qemu)
#     make pi5        # build kernel_2712.img   (Raspberry Pi 5 / BCM2712)
#     make pi5-min    # build kernel_xinu.img   (Pi 5 first-light bring-up)
#     make pi4        # build kernel8.img       (Raspberry Pi 4 / BCM2711)
#     make qemu       # build kernel_virt.img   (QEMU -M virt)
#     make qemu-run   # build + launch QEMU (Ctrl-A X to quit)
#     make qemu-smoke # canned-input smoke run
#     make install_pi5 SDCARD=/Volumes
#     make clean
#
# Every goal is simply forwarded into compile/ via `$(MAKE) -C compile`.

COMPILE_DIR := compile

# Bare `make` builds everything, matching compile/Makefile's default.
.DEFAULT_GOAL := all

.PHONY: all pi4 pi5 pi5-min qemu qemu-run qemu-smoke \
        install install_pi4 install_pi5 clean help

all pi4 pi5 pi5-min qemu qemu-run qemu-smoke \
install install_pi4 install_pi5 clean help:
	$(MAKE) -C $(COMPILE_DIR) $@

# Forward any other goal (e.g. a specific image name like kernel_2712.img)
# down to compile/ as well, so nothing the inner Makefile knows about breaks
# when invoked from the root.
%:
	$(MAKE) -C $(COMPILE_DIR) $@
