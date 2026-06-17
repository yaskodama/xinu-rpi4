# Pi4 (xinu-rpi4) — Next-Session Handoff

Last updated: **2026-06-16**.  Branch **`main`**, HEAD **`c16321f`** (pushed to
`github.com/yaskodama/xinu-rpi4`).  Older 2026-06-10 notes kept below for reference.

---

## ★ 2026-06-16 session — on-screen BASIC window (DONE) + koch cursor-freeze (OPEN)

### Shipped (merged to `main`, pushed)
- **`6b94c0b` feat(pi4): on-screen BASIC window + active-window input routing** —
  ported the Pi 3 desktop's BASIC to the Pi 4 WM.  New files:
  - `device/video/basic.c` (interpreter core, `double` number type — built with
    `PI4_FP_CFLAGS`, see the explicit `basic.o` rule in `compile/Makefile`).
  - `device/video/basicwin.c` + `include/basicwin.h` (wm-managed window: text
    ring, line editor, toolbar buttons FILES/LIST/hanoi/bsort/fizz/qsort/koch,
    LINE/CIRCLE/PLOT → shared `gfx_*` display list).
  - `device/video/wm.c` + `include/wm.h`: `window.on_click` seam, `wm_active()`
    (keyboard routes to last-clicked window), press-edge click detect,
    `wm_cursor_repaint()` / `wm_cursor_after_blit()` (pump mouse + re-stamp
    cursor during a blocking BASIC graphics loop).
  - `device/usb/xhci/xhci.c`: full Shift/Ctrl/nav-key translation (`x_deliver_key`,
    `x_shift_char`), `xhci_poll_ctrl_c()` (sticky Ctrl-C for the RUN loop).
  - `system/tcp_server.c`: `/dhcp` endpoint (DHCP lease report).
  - `tools/remote_chainload.py`: require `ok off=<off>` ack + 5× retry per chunk
    (first POST after handoff could be silently dropped → corrupt staged image).
- **`c16321f` docs(README)**: real-hardware HDMI screenshot
  `doc/pi4-basic-koch.jpg` (BASIC koch fractal).  ★EXIF/GPS stripped + resized
  (`magick … -resize 1600x -strip -quality 85`); original had Tokyo GPS coords.
- Verified on **real hardware** (photo + `/windows` shows the "BASIC" window).
  Current-source build: `compile/kernel8.img` **md5 `5c05d662`**, 1,925,976 B.

### ★ OPEN BUG — mouse cursor freezes while `koch` (any long BASIC gfx run) runs
- Repro: BASIC window → click **koch** (or `RUN "koch"`).  Cursor stops moving.
- The koch sample (`S_koch` in `basic.c`) is a `*LOOP / WAIT 0.1 / GOTO *LOOP`
  loop, so it SHOULD pump every 0.1 s:
  `WAIT` → `bw_pause()` (basicwin.c) → `bw_present_gfx()` → `wm_cursor_after_blit()`
  → `xhci_mouse_pump()` (updates `cursor_x/y`) + `cursor_vis_show()`.
- **Lead / prime suspect**: `xhci_mouse_pump()` drains the HID event ring but the
  **mouse** interrupt-IN transfer likely is not being **re-armed** during a
  blocking run (the diff re-arms only the *keyboard* slot: `x_hid_arm(x_kbd_slot,
  x_kbd_dci)` — check whether the mouse slot gets an equivalent `x_hid_arm(
  x_mouse_slot, x_mouse_dci)` on its branch).  Without re-arming, the first motion
  report is delivered then no more arrive → cursor stuck.  ALSO check the long
  no-WAIT path: `*REDRAW`/`*KOCH` recursion (D up to 10 → ~4^10 segments) runs with
  no `WAIT`, so no pump at all during a redraw — consider pumping inside `bw_line`
  every N segments.
- Files to edit: `device/usb/xhci/xhci.c` (`xhci_mouse_pump`, `x_hid_arm` for the
  mouse branch), `device/video/basicwin.c` (`bw_line` periodic pump option).

### Deploy gotcha hit this session
- `remote_chainload.py` aborted on chunk 0 (`<URLError>`) — the **HTTP worker had
  wedged** (known: `/usbdiag` went 200→000, TCP :80 NO CONNECT).  The new retry
  guard correctly refused to stage a corrupt image (SD untouched).  Fix =
  **physical power-cycle** (then it boots SD; re-chainload `5c05d662` once HTTP is
  healthy).  After the user's power-cycle the device returned healthy (HTTP 200,
  BASIC window present).

---


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

- Build: `cd compile && make pi4` → `compile/kernel8.img` (Pi4 = `kernel8.img`).
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
