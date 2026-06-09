# Pi4 (xinu-rpi4) — Next-Session Handoff

Last updated: 2026-06-10.  Branch `feat/wifi-pi4`, HEAD **`1cc794b`** (all pushed to
`github.com/yaskodama/xinu-rpi4`).

## What this session built (bare-metal Pi4 / BCM2711, AArch64)

A full USB-A input stack **and** virtual memory, all verified on real hardware:

| Commit | What |
|--------|------|
| `cc16e76` | BCM2711 PCIe RC bring-up + VL805 xHCI reachable + device enumeration |
| `72bcd13` | USB hub enumeration (route string + TT) + HID endpoint setup + report pump |
| `4fc7d08` | **Working USB mouse + keyboard** via the USB-2.0 hub's TT |
| `7f4bcc7` | Smooth cursor + window move / resize / active-highlight / resize-grip |
| `1cc794b` | **Demand-paged virtual memory** (page-fault driven) |

## Device / connection

- Pi4 at **192.168.3.100** (ethernet, static; HTTP server in `system/tcp_server.c`).
- **Serial**: `/dev/cu.usbserial-1120`, **115200 8N1**.  ★Rule: **open `cat` FIRST, then
  `stty`** (`cat $PORT >log & ; sleep 1 ; stty -f $PORT 115200 cs8 -cstopb -parenb raw -echo`).
  stty-before-cat garbles every baud (cat's open resets termios).  RX is dead
  (no keyboard-into-serial); TX works.  Firmware sets PL011 to 103448 baud but
  reading at 115200 is clean.
- **★USB mouse/keyboard MUST be on the USB-2.0 (black) ports.**  They route through
  the Genesys USB2 hub (a real HS hub with a working TT), so LS interrupt-IN split
  transactions deliver.  On the USB-3.0 (blue) ports they sit behind the VIA USB3
  hub whose TT path does NOT deliver the periodic transfer (cursor won't move).

## Build / deploy / test

- Build: `cd compile && make pi4` → `compile/kernel8.img` (Pi4 = `kernel8.img`;
  `kernel_2712.img` is the Pi5 build, ignore).
- **SD boot (persistent, reliable)**: SD volume `/Volumes/bootfs`, kernel file
  `kernel8.img`.  `cp compile/kernel8.img /Volumes/bootfs/kernel8.img` + md5-verify +
  eject; **preserve `config.txt`** (do not touch).  Current SD = md5 `d8302751`
  (demand-paging build); pre-VM backup at `/Volumes/bootfs/kernel8.img.bak-pre-vm`.
- **Chainload (fast iterate, RAM-only, no SD swap)**:
  `python3 tools/remote_chainload.py 192.168.3.100 compile/kernel8.img`
  (~50 s upload; a bad image just needs a power cycle — SD untouched).
  ★Use the python tool, NOT hand-rolled curl (the GET `go` didn't fire reliably).
  Needs the device's HTTP server **responsive** to upload.
- Diagnostics (HTTP): `/usbdiag` (pump_calls/mfindex/ep_state/mouse_reports/buf/
  xfer_events/last_cc), `/pcie-init`, `/pcie-enum`, `/fault`, `/shell?cmd=<cmd>`,
  `/reboot` (BCM2711 watchdog), `/chainload`.
- VM demo: `curl "http://192.168.3.100/shell?cmd=vmdemand"` → touches 64 pages in
  the demand window; expect `faults this run = 0x40, total mapped = 0x40, readback
  OK` first run, `0x0` faults second run, `/fault` fault_count=0.
  Also `/shell?cmd=vmtest` = VA≠PA remap demo.

## Key gotchas / hard-won facts

- **VL805 firmware**: on an SD (non-USB-MSD) boot the bootloader leaves PCIe RC in
  reset and VL805 has no firmware → CNR stuck, HCRST never completes.  Fix:
  `NOTIFY_XHCI_RESET` mailbox tag `0x00030058` with **devid `0x100000`**
  (PCI bus1<<20|slot0<<15|func0<<12; Linux `reset-raspberrypi.c` value; devid=0 is
  a no-op).  Done at boot in `loader/main.c` after PCIe link-up + enum.
- **uart_puts DEADLOCKS in the HTTP-worker context** (screen/shellwin fanout lock).
  All PCIe/xHCI bring-up logs run at **boot** (single-threaded), observed over serial;
  runtime diagnostics use HTTP counters (`/usbdiag`), never uart from a worker.
- **HTTP worker can wedge**: a sync `[EXC]` puts the faulting proc in `recover_spin`;
  ICMP/ping keep working (separate thread) but HTTP stops responding → chainload
  can't upload → **power-cycle required**.  (Seen intermittently on in-RAM builds;
  a clean SD boot of `1cc794b` showed `fault_count=0`.)
- **D-cache is OFF** (`sctlr` C=0) for DMA coherency (GENET/USB/mailbox); MMU + I-cache
  on.  This makes the framebuffer slow → the 20 fps full-scene render is the perf
  bottleneck (root cause of the cursor/keyboard "smoothness" asks below).
- Demand-paging window: VA `0x80000000`..`0x80400000` (4 MiB), L1 index 2; 512-frame
  pool (`vmd_pool`); `vm_fault()` in `system/mmu.c`; retry path = `sync_entry`
  (full context save) → `sync_dispatch_c` → `vm_fault` → return → `eret` retries.

## Pending / not done

- **3 UX requests left unaddressed** (root cause = the 20 fps full-scene repaint
  freezing the cursor/echo each frame; needs render-rate/region rework or D-cache):
  1. Shell window: allow input down to the very bottom line (`sh_draw` in
     `device/video/shellwin.c`; bottom margin `-7` + integer `max_rows` wastes ~1 line).
  2. Keyboard input smoother (auto-repeat not implemented; echo latency = scene fps).
  3. Mouse cursor even smoother (cursor freezes during each full render/`video_present`).
- VM: no swap/eviction (OOM after 512 frames), single demand window, not per-process.
- Intermittent `[EXC]` (data-abort write to `.text`, FAR≈0x80638) seen on some in-RAM
  builds; source not pinned (read `/fault` after a fresh boot if it recurs).

## Memory references
`reference_bcm2711_pcie_bringup.md` (PCIe/xHCI/TT/firmware detail),
`project_xinu_rpi4_remote.md`, `project_xinu_rpi4_wifi.md`.  Linux source for
reference: `~/projects/linux-rpi` (sparse, rpi-6.18.y).
