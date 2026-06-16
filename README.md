# xinu-rpi4

Embedded Xinu port for the **Raspberry Pi 4 (BCM2711, Cortex-A72, AArch64)**,
running on real hardware. The same source tree also cross-builds for the
QEMU `virt` machine.

Bootstrapped from [`yaskodama/xinu-rpi`](https://github.com/yaskodama/xinu-rpi)
(32-bit arm-qemu / arm-rpi) and the AArch64 boot pattern from
[`radlyeel/leex`](https://github.com/radlyeel/leex). It has since grown from a
serial hello-world into a small interactive system with an HDMI window manager,
USB-A input, virtual memory, networking (wired + WiFi mesh), an on-device C JIT,
an actor runtime, and an HTTP control plane.

![Xinu on a Raspberry Pi 4 over HDMI: the window manager showing the runtime
monitor, UART shell, live actors, system status, graphics, and an on-screen
BASIC window drawing a Koch-snowflake fractal.](doc/pi4-basic-koch.jpg)

*Real hardware (Pi 4 / BCM2711) over HDMI — the window manager with the runtime
monitor, UART shell, live-actor and system-status panels, and the on-screen
**BASIC** window running its `koch` sample to draw a Koch snowflake.*

## What works

**Boot & core kernel**
- AArch64 leex-style stub → BSS clear → `kernel_main` → window manager + shell.
- **MMU**: identity map + a **demand-paged virtual-memory window** (page-fault
  driven, VA `0x80000000`..`0x80400000`, 512-frame pool). D-cache off for DMA
  coherency; I-cache + MMU on.
- **Scheduler**: cooperative AArch64 `ctxsw` (S0) **and** preemptive 100 Hz
  round-robin off the GIC-400 + generic timer (S1).
- First-fit kernel heap (`getmem`/`freemem`), 16-byte aligned, coalescing.

**Devices (BCM2711, on real hardware)**
- **HDMI framebuffer + window manager** — a 1280×960 virtual desktop with a
  movable/resizable shell window, smooth mouse cursor, and live monitors.
- **USB-A input** — BCM2711 PCIe RC bring-up + VL805 xHCI + hub enumeration
  (route string + TT) → **working USB mouse + keyboard** (on the USB-2.0 black
  ports; see *Known limits*).
- **Ethernet (GENET)** — static `192.168.3.100`, ARP + ICMP (pingable),
  16-slot RX ring.
- **WiFi (BCM43455)** — scan / WPA2 join / DHCP / ping / NTP / DNS / HTTP /
  TFTP / netboot, plus **MANET ad-hoc (IBSS) + on-demand AODV multi-hop mesh**
  (the same stack the Pi 4 + Pi 3 nodes use in the drone-HIL demo).

**Runtime & tooling**
- **On-device C JIT** (`cc`) — compiles a C subset to native AArch64 and runs it.
- **AIPL actor runtime** — resident actors, message send, a select/receive demo,
  and an actor-pool GC.
- **Embedded tiny LLM** (`llm`) — a baked-in transformer for on-device text gen.
- **HTTP control plane** (`system/tcp_server.c`) — run shell commands, upload &
  chainload a new kernel, drive diagnostics, all over the wire.
- **Network kernel update** — `tools/remote_chainload.py` swaps the running
  kernel in RAM with no SD card swap.

## Target hardware

|              | **Pi 4 (this repo)** | QEMU virt            |
|--------------|----------------------|----------------------|
| SoC          | **BCM2711**          | QEMU `virt`          |
| Cores        | **Cortex-A72 ×4**    | `-cpu cortex-a76`    |
| MMIO base    | **0xFE000000**       | —                    |
| I/O          | **GENET Ethernet + VL805 PCIe xHCI (USB-A) + BCM43455 WiFi** | virtio / PL011 |
| Firmware img | **`kernel8.img`**    | `kernel_virt.img`    |
| UART base    | **`0xFE201000`**     | `0x09000000`         |
| Load address | **`0x80000`**        | `0x40080000`         |

## Build

```sh
# Mac (Homebrew AArch64 cross toolchain — pick either):
brew install aarch64-elf-gcc           # GNU
brew install --cask gcc-arm-embedded   # ARM-supplied

cd compile
make pi4            # → compile/kernel8.img   (real Pi 4)
make qemu           # → compile/kernel_virt.img (QEMU virt)
make                # = pi4 + qemu
```

Override the toolchain location with `make pi4 GCCPATH=...`.

## Deploy

**SD card (persistent):**

```sh
cd compile
make install_pi4 SDCARD=/Volumes      # copies kernel8.img + config_pi4.txt
# or by hand: cp compile/kernel8.img /Volumes/bootfs/kernel8.img  (preserve config.txt)
```

The card needs the stock Raspberry Pi OS firmware blobs (`bootcode.bin`,
`start4.elf`, …) plus `kernel8.img` and `config_pi4.txt` (→ `config.txt`).

**Network chainload (fast iterate, RAM-only, no SD swap):**

```sh
python3 tools/remote_chainload.py 192.168.3.100 compile/kernel8.img
# ~50 s upload; a bad image just needs a power cycle (the SD is untouched).
# Requires the device's HTTP server to be responsive.
```

## Console

- **Serial**: 3.3 V USB-serial on header pins 8 (TXD→GPIO14) / 10 (RXD→GPIO15)
  / 6 (GND), **115200 8N1**. `screen /dev/tty.usbserial-XXXX 115200`.
- **HDMI**: the window-manager desktop with the interactive shell window.
- **Remote**: `curl "http://192.168.3.100/shell?cmd=help"` runs a shell command
  over HTTP and returns its output.

The boot banner (over serial) looks like:

```
================================================
  Xinu Pi4 hello (AArch64, BCM2711, kernel8.img)
  PL011 UART0 @ 0xFE201000, 115200 8N1
================================================
xinu-pi4$ _
```

## Shell commands

| Area | Commands |
|------|----------|
| Files | `pwd` `ls` `cd` `mkdir` `rmdir` `touch` `cat` `write` `edit` `rm` `cp` `mv` `tree` |
| Compile / run | `cc <file.c>` (JIT C → AArch64) |
| Actors / AIPL | `aload` `amsg` `actordemo` `selectdemo` |
| Memory / VM | `mem` `vmtest` (VA≠PA remap) `vmdemand` (demand paging) |
| Scheduler | `procdemo` `pingpong` `preempt` `ticks` `ps` |
| Networking | `wifi probe\|scan\|up\|adhoc\|aodv\|ping\|…` `rxstat` `tcpstat` |
| Devices | `usb` (xHCI/DWC2 diag) `peek` `pan` `view` `autopan` |
| LLM | `llm [prompt]` |
| Misc | `help` `?` `echo` `hello` `uptime` `halt` `reboot` |

WiFi connection and **multi-node mesh** (`wifi adhoc` / `wifi aodv`) are
documented in the user manuals — see *Documentation* below.

## HTTP control plane

`system/tcp_server.c` serves (default port 80) a set of introspection/control
routes:

```
GET  /shell?cmd=<cmd>     run a shell command, return its output
POST /compile            body = C source; JIT-compile & run
GET  /chat?...           on-device LLM chat
GET  /actor , /send      AIPL actor inventory / message send
GET  /gc , /jitstats     actor-pool GC + JIT counters
GET  /usbdiag            xHCI/HID counters (pump/mfindex/ep_state/reports/…)
GET  /pcie-init,/pcie-enum  PCIe RC bring-up + device enumeration
GET  /fault              page-fault / exception counters
GET  /mmio-read,/mmio-write,/mmio-sweep   raw MMIO peek/poke
POST /chainload          upload + jump to a new kernel (no SD swap)
POST /reboot             BCM2711 watchdog reset
```

## QEMU

```sh
cd compile
make qemu          # interactive — Ctrl-A X to quit
make qemu-smoke    # canned commands → qemu-smoke.log
```

The QEMU `virt` build uses `-cpu cortex-a76` (MIDR `0x414fd0b1`); the same source
runs on the Pi 4's Cortex-A72 on hardware. No networking / USB / SD / HDMI under
QEMU (hardware not modelled).

## Layout

```
xinu-rpi4/
├── compile/        # build dir — `make pi4` / `make qemu`; link*.ld; obj/<variant>/
├── loader/         # boot.S (AArch64 stub) + main.c (init + WM + shell handoff)
├── system/         # proc/ctxsw, mmu (+ demand paging), tcp_server, exceptions
├── mem/            # first-fit heap
├── shell/          # bare-metal REPL + command handlers
├── device/         # uart, video (HDMI + window manager), usb/xhci, gic, timer,
│                   #   genet (ethernet), wifi (BCM43455), sd, mbox
├── cc/  llm/  fs/  # C JIT, embedded LLM, in-RAM filesystem
├── network/        # arp / ipv4 / icmp / net (xinu-raz stack)
├── sdcard/         # config_pi4.txt
├── tools/          # remote_chainload.py
└── doc/            # LaTeX user manuals (EN + JA) → PDF
```

## Documentation

- **User manuals** (operator-facing, EN + JA): `USERS_MANUAL_EN.md` /
  `USERS_MANUAL_JA.md`, plus typeset PDFs under `doc/` (`doc/Makefile`,
  lualatex). They cover build, deploy, the shell, **WiFi connection**, and
  **multi-node mesh networking**.
- Session handoff / hard-won hardware facts: `NEXT_SESSION_PI4.md`.

## Known limits

- USB mouse/keyboard must be on the **USB-2.0 (black) ports** — the USB-3.0
  (blue) hub's TT does not deliver the periodic (interrupt-IN) transfer.
- The full-scene HDMI repaint runs ~20 fps (D-cache off), which caps cursor /
  keyboard echo smoothness.
- Demand-paging has no swap/eviction (OOM after 512 frames), a single window,
  and is not per-process.
- An HTTP worker can wedge on a faulting request (ICMP/ping keep working); a
  power cycle clears it.
- WiFi / ad-hoc are not restored after a reboot — re-run `wifi probe` + `wifi up`
  (or `wifi adhoc`) each boot.

## License

Inherits from upstream Xinu / leex (BSD-style). See `LICENSE` once the
source-of-truth license file is added.
