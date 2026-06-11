# compile/Makefile — top-level build for xinu-rpi4 (Raspberry Pi 4).
#
# Mirrors the layout convention used by github.com/davidxyz/xinuPi
# (and Embedded Xinu in general): sources live under per-subsystem
# directories one level up (loader/, mem/, system/, shell/, device/),
# headers live in ../include/, and *every* build artefact ends up in
# this directory (objects in obj/<variant>/, ELFs + flat images at
# the root).
#
# Variants:
#   pi4   Raspberry Pi 4 (BCM2711, Cortex-A72)  -> kernel8.img   (primary)
#   qemu  QEMU `-M virt -cpu cortex-a76`         -> kernel_virt.img
#   pi5   Raspberry Pi 5 (BCM2712, Cortex-A76)  -> kernel_2712.img
#         (legacy / opt-in only; this is a Pi 4-dedicated project)
#
# Targets:
#   make             pi4 + qemu        (default; pi5 is opt-in)
#   make pi4         kernel8.img       (default Pi 4 firmware kernel name)
#   make qemu        interactive QEMU virt (Ctrl-A X to quit)
#   make qemu-smoke  canned-input smoke run; writes qemu-smoke.log
#   make pi5         kernel_2712.img   (legacy Pi 5 image, opt-in)
#   make install_pi4 SDCARD=/Volumes
#   make install_pi5 SDCARD=/Volumes  (legacy)
#   make clean
#
# Run from this directory:
#     cd compile && make pi4
#
# Source tree (relative to ..):
#   loader/        boot.S + main.c   (AArch64 entry + kernel_main)
#   mem/           memory.c          (first-fit getmem/freemem)
#   system/        proc.c + ctxsw.S  (proctab + scheduler + ctxsw)
#   shell/         shell.c           (bare-metal REPL)
#   device/uart/   uart.c            (PL011 UART0 driver)
#   device/mbox/   mbox.c            (VC mailbox property channel)
#   device/video/  video.c           (HDMI framebuffer + 8x8 font console)
#   include/       *.h               (shared headers)

# ----- toolchain ------------------------------------------------------
GCCPATH ?= /opt/homebrew
CROSS   ?= $(shell test -x $(GCCPATH)/bin/aarch64-elf-gcc && \
                   echo aarch64-elf- || echo aarch64-none-elf-)
CC       = $(GCCPATH)/bin/$(CROSS)gcc
LD       = $(GCCPATH)/bin/$(CROSS)ld
OBJCOPY  = $(GCCPATH)/bin/$(CROSS)objcopy

# ----- source discovery -----------------------------------------------
COMPONENTS = loader mem system shell fs cc llm \
             device/uart device/mbox device/video device/sd device/usb \
             device/usb/xhci device/gic device/timer device/genet device/wifi \
             network/arp network/net network/netaddr network/ipv4 network/icmp
# Step 2 of NET-E: building xinu-raz network/{arp,net,netaddr,ipv4,icmp}
# on top of the xinu_compat layer.
# extern/uspi/lib disabled — USPi only drives DWC2 (Pi 4 USB-C OTG).
# Pi 4 USB-A keyboards / mice route through VL805 PCIe xHCI, which
# needs a hand-rolled driver under device/usb/xhci/.

C_SRCS := $(foreach d,$(COMPONENTS),$(wildcard ../$(d)/*.c))
S_SRCS := $(foreach d,$(COMPONENTS),$(wildcard ../$(d)/*.S))

# Per-variant object trees keep the three builds isolated.
PI5_OBJS  := $(patsubst %.c,obj/pi5/%.o,$(notdir $(C_SRCS))) \
             $(patsubst %.S,obj/pi5/%.o,$(notdir $(S_SRCS)))
PI4_OBJS  := $(patsubst %.c,obj/pi4/%.o,$(notdir $(C_SRCS))) \
             $(patsubst %.S,obj/pi4/%.o,$(notdir $(S_SRCS)))
QEMU_OBJS := $(patsubst %.c,obj/qemu/%.o,$(notdir $(C_SRCS))) \
             $(patsubst %.S,obj/qemu/%.o,$(notdir $(S_SRCS)))

VPATH := $(addprefix ../,$(COMPONENTS))

# ----- flags ----------------------------------------------------------
INCLUDE = -I../include -I../extern/xinu-raz-include
# -mstrict-align: the MMU is off, so all DRAM is Device-nGnRnE on AArch64
# where unaligned accesses fault.  Without this, GCC -O2 happily folds
# byte loops and byte-OR idioms into wider str/ldr/stp that are only
# 4-byte aligned and data-abort at runtime (this is what silently hung
# the box on the first TCP segment, and again inside actor_init()).
# -mstrict-align makes the compiler never emit an unaligned access, which
# removes that whole class of landmine kernel-wide.
COMMON  = -Wall -O2 -ffreestanding -nostdinc -nostdlib -nostartfiles \
          -mgeneral-regs-only -mstrict-align $(INCLUDE) -MMD -MP

# Pi 5 (BCM2712, Cortex-A76).  Peripheral base 0x107C000000; mailbox at
# 0x107C00B880 (best-effort).  Serial console = GPIO14/15 = RP1 UART0 at
# BCM2712-side 0x1F00030000 (PCIe window to RP1; needs pciex4_reset=0 in
# config.txt).  HEAP_END 1 GiB conservative for the smallest Pi 5.
# UART0_BASE = the BCM2712 on-SoC dedicated DEBUG UART (0x107D001000), NOT
# the RP1 UART0 on GPIO14/15 (0x1F00030000).  The RP1 UART sits behind the
# PCIe window, so uart_init() — which runs with the MMU off, before any PCIe
# mapping — DATA-ABORTS the moment it pokes 0x1F00030000, killing the kernel
# before its first byte of output ("nothing after BL31" over serial).  The
# debug UART is on-SoC and PCIe-independent, so it is always safe and is the
# one wired to the 3-pin JST-SH debug header / Debug Probe.
PI5_CFLAGS  = $(COMMON) -mcpu=cortex-a76 \
              -DUART0_BASE=0x107D001000UL \
              -DUART_TRUST_FW_BAUD \
              -DMBOX_BASE=0x107C00B880UL \
              -DHEAP_END=0x40000000UL \
              -DBOARD_NAME=\"Pi5\" \
              -DSOC_NAME=\"BCM2712\" \
              -DKERNEL_NAME=\"kernel_2712.img\"

# Pi 4 (BCM2711, Cortex-A72).  Peripheral base 0xFE000000; PL011 at
# 0xFE201000; mailbox at 0xFE00B880 (canonical, well-documented).
# HEAP_END 1 GiB suits the 1/2/4 GiB SKUs; 8 GiB Pi 4 has more but
# we don't lose anything by capping low.
PI4_CFLAGS  = $(COMMON) -mcpu=cortex-a72 \
              -DUART0_BASE=0xFE201000UL \
              -DMBOX_BASE=0xFE00B880UL \
              -DSD_BASE=0xFE340000UL \
              -DUSB_BASE=0xFE980000UL \
              -DGIC_BASE=0xFF840000UL \
              -DPCIE_BASE=0xFD500000UL \
              -DGENET_BASE=0xFD580000UL \
              -DGPIO_BASE=0xFE200000UL \
              -DHEAP_END=0x40000000UL \
              -DBOARD_NAME=\"Pi4\" \
              -DSOC_NAME=\"BCM2711\" \
              -DKERNEL_NAME=\"kernel8.img\"

# QEMU `-M virt -cpu cortex-a76`.  PL011 at 0x09000000, kernel loaded
# at base+0x80000=0x40080000, 256 MiB RAM.  SKIP_MBOX bypasses both
# the mailbox driver and the early-paint diagnostic so we don't
# touch unmapped MMIO that would data-abort without exception vectors.
QEMU_CFLAGS = $(COMMON) -mcpu=cortex-a76 \
              -DUART0_BASE=0x09000000UL \
              -DHEAP_END=0x50000000UL \
              -DSKIP_MBOX \
              -DBOARD_NAME=\"virt\" \
              -DSOC_NAME=\"QEMU\" \
              -DKERNEL_NAME=\"kernel_virt.img\"

# ----- pattern rules --------------------------------------------------
obj/pi5 obj/pi4 obj/qemu:
	@mkdir -p $@

# cc.c carries the value_t runtime, which needs floating point for AIPL
# float values — build it WITHOUT -mgeneral-regs-only (FP enabled at EL1
# via CPACR in mmu_init).  Everything else stays integer-only.
PI5_FP_CFLAGS  := $(filter-out -mgeneral-regs-only,$(PI5_CFLAGS))
PI4_FP_CFLAGS  := $(filter-out -mgeneral-regs-only,$(PI4_CFLAGS))
QEMU_FP_CFLAGS := $(filter-out -mgeneral-regs-only,$(QEMU_CFLAGS))
obj/pi5/cc.o:  cc.c | obj/pi5  ; $(CC) $(PI5_FP_CFLAGS)  -c $< -o $@
obj/pi4/cc.o:  cc.c | obj/pi4  ; $(CC) $(PI4_FP_CFLAGS)  -c $< -o $@
obj/qemu/cc.o: cc.c | obj/qemu ; $(CC) $(QEMU_FP_CFLAGS) -c $< -o $@

# llm.c runs transformer inference — needs FP/SIMD, like cc.o.
obj/pi5/llm.o:  llm.c | obj/pi5  ; $(CC) $(PI5_FP_CFLAGS)  -c $< -o $@
obj/pi4/llm.o:  llm.c | obj/pi4  ; $(CC) $(PI4_FP_CFLAGS)  -c $< -o $@
obj/qemu/llm.o: llm.c | obj/qemu ; $(CC) $(QEMU_FP_CFLAGS) -c $< -o $@

# Pi 5 OMITS the ~1 MB embedded LLM model (blob.S -DLLM_NO_MODEL).  With the
# model baked in, the loaded kernel image grows past ~1.5 MB and the Pi 5 no
# longer boots: the firmware loads kernel_2712.img, BL31 runs, then the kernel
# produces no output (confirmed over the dedicated debug UART).  llm.c is still
# compiled so the llm/chat symbols resolve, but model_data is an empty
# placeholder, so the llm/chat commands are inert on the Pi 5.  Pi 4 and QEMU
# keep the full model (it both boots and runs inference there).
obj/pi5/blob.o:  blob.S | obj/pi5  ; $(CC) $(PI5_CFLAGS) -DLLM_NO_MODEL -c $< -o $@

obj/pi5/%.o: %.c  | obj/pi5  ; $(CC) $(PI5_CFLAGS)  -c $< -o $@
obj/pi5/%.o: %.S  | obj/pi5  ; $(CC) $(PI5_CFLAGS)  -c $< -o $@
obj/pi4/%.o: %.c  | obj/pi4  ; $(CC) $(PI4_CFLAGS)  -c $< -o $@
obj/pi4/%.o: %.S  | obj/pi4  ; $(CC) $(PI4_CFLAGS)  -c $< -o $@
obj/qemu/%.o: %.c | obj/qemu ; $(CC) $(QEMU_CFLAGS) -c $< -o $@
obj/qemu/%.o: %.S | obj/qemu ; $(CC) $(QEMU_CFLAGS) -c $< -o $@

# ----- images ---------------------------------------------------------
# Default build is Pi 4 + QEMU.  The legacy `pi5` image stays buildable
# via an explicit `make pi5` but is not part of `all`.
all: pi4 qemu

pi5 kernel_2712.img: $(PI5_OBJS) link.ld
	$(LD) -nostdlib $(PI5_OBJS) -T link.ld -o kernel_2712.elf
	$(OBJCOPY) -O binary kernel_2712.elf kernel_2712.img
	@ls -la kernel_2712.img

pi4 kernel8.img: $(PI4_OBJS) link.ld
	$(LD) -nostdlib $(PI4_OBJS) -T link.ld -o kernel8.elf
	$(OBJCOPY) -O binary kernel8.elf kernel8.img
	@ls -la kernel8.img

# ----- minimal Pi 5 first-light kernel (serial bring-up only) --------
# Just boot.S + firstlight.c.  No video / mbox / RP1 / net — the smallest
# possible image that proves the BCM2712 dedicated debug UART works.
# Output image is kernel_xinu.img so it never clobbers a Linux/full image.
obj/pi5/firstlight.o: ../firstlight/firstlight.c | obj/pi5
	$(CC) $(PI5_CFLAGS) -c $< -o $@

PI5MIN_OBJS = obj/pi5/firstlight.o obj/pi5/boot.o obj/pi5/mbox.o obj/pi5/font8x8.o
pi5-min kernel_xinu.img: $(PI5MIN_OBJS) link.ld
	$(LD) -nostdlib $(PI5MIN_OBJS) -T link.ld -o kernel_xinu.elf
	$(OBJCOPY) -O binary kernel_xinu.elf kernel_xinu.img
	@ls -la kernel_xinu.img

qemu kernel_virt.img: $(QEMU_OBJS) link_virt.ld
	$(LD) -nostdlib $(QEMU_OBJS) -T link_virt.ld -o kernel_virt.elf
	$(OBJCOPY) -O binary kernel_virt.elf kernel_virt.img
	@ls -la kernel_virt.img

# ----- QEMU run targets ----------------------------------------------
QEMU             ?= qemu-system-aarch64
QEMU_COMMON       = -M virt -cpu cortex-a76 -m 256M -kernel kernel_virt.img
QEMU_FLAGS        = $(QEMU_COMMON) -nographic
QEMU_SMOKE_FLAGS  = $(QEMU_COMMON) -display none -serial stdio

qemu-run: kernel_virt.img
	@echo ">>> launching QEMU virt (Ctrl-A X to quit) <<<"
	$(QEMU) $(QEMU_FLAGS)

qemu-smoke: kernel_virt.img
	printf '\n\nhello\nmem\npeek 0x09000018\nuptime\nps\npingpong 3\nprocdemo 3\nps\necho --- smoke ok ---\nhalt\n' | \
	  timeout 15 $(QEMU) $(QEMU_SMOKE_FLAGS) | tee qemu-smoke.log

# ----- install onto a Pi 4 / Pi 5 SD card ----------------------------
SDCARD ?= /Volumes
DEST    = $(SDCARD)/bootfs

install_pi4: kernel8.img
	sudo rm -f $(DEST)/kernel_*.img $(DEST)/kernel8.img
	sudo cp kernel8.img $(DEST)/
	sudo cp ../sdcard/config_pi4.txt $(DEST)/config.txt
	sync
	sudo umount $(DEST) || true

install_pi5: kernel_2712.img
	sudo rm -f $(DEST)/kernel_*.img $(DEST)/kernel8.img
	sudo cp kernel_2712.img $(DEST)/
	sudo cp ../sdcard/config.txt $(DEST)/
	sync
	sudo umount $(DEST) || true

# `make install` ships the Pi 4 image (this is a Pi 4-dedicated project).
# Use `make install_pi5` explicitly for the legacy Pi 5 image.
install: install_pi4

# ----- housekeeping --------------------------------------------------
clean:
	rm -rf obj kernel_*.elf kernel_*.img kernel8.elf kernel8.img qemu-smoke.log

.PHONY: all pi4 pi5 pi5-min qemu qemu-run qemu-smoke install install_pi4 install_pi5 clean

# Auto-generated header dependencies (-MMD): a change to e.g. a struct in a
# header now recompiles every .c that includes it, instead of leaving stale
# objects with a mismatched layout (which silently boot-crashed the Pi).
-include $(wildcard obj/pi4/*.d obj/pi5/*.d obj/qemu/*.d)
