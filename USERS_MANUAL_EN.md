# Xinu xinu-rpi4 User's Manual (English)

Embedded Xinu port for the Raspberry Pi 4 (BCM2711 / Cortex-A72, AArch64).
The same source tree also cross-builds for the QEMU `virt` machine. This
manual is written for **operators** — for the development roadmap see
`README.md`.

----------------------------------------------------------------------

## 1. Targets and Build Artefacts

| Target | Board / machine                | Output image       | UART0 base     |
|--------|--------------------------------|--------------------|----------------|
| `pi4`  | Raspberry Pi 4 (BCM2711)       | `kernel8.img`      | `0xFE201000`   |
| `qemu` | `qemu-system-aarch64 -M virt -cpu cortex-a76` | `kernel_virt.img` | `0x09000000` |

The load address is `0x80000` for Pi 4 and `0x40080000` for QEMU.
Both images come from one source tree, distinguished by `-D<TARGET>`
macros and a per-target linker script.

### 1.1 Feature Matrix (read this first!)

Not every feature works on every target. Several subsystems run only on
real hardware (Pi 4):

| Subsystem                        | Pi 4 | QEMU virt | Notes                                            |
|----------------------------------|:----:|:---------:|--------------------------------------------------|
| PL011 UART0 shell                |  ✅  |    ✅     | The common substrate                              |
| Cooperative scheduler (S0)       |  ✅  |    ✅     | `procdemo` / `pingpong`                           |
| Preemptive scheduler (S1)        |  ✅  |    ⏳     | 100 Hz timer tick works on Pi 4 (`ticks` cmd)     |
| HDMI framebuffer                 |  ✅  |    —      | Usable on Pi 4                                     |
| Window manager / virtual desktop |  ✅  |    —      | 1280×960 desktop is Pi 4 only                     |
| SD card + FAT32                  |  ✅  |    —      | EMMC driver is BCM2711-specific                   |
| Ethernet (GENET)                 |  ✅  |    —      | NET-E done (BCM2711 GENET)                         |
| **USB (keyboard / HID)**         |  ✅  |    —      | Via VL805 PCIe xHCI (USB-2.0 ports — see §10/§11)  |
| DHCP / TCP                       |  ❌  |    —      | Code present but dispatch OFF (§7.4)              |

> ⚠ The source tree contains `device/genet/`, `device/sd/`,
> `device/usb/xhci/` etc., initialised against **Pi 4 (BCM2711) register
> addresses**. The `-D<BASE>` macros (`GENET_BASE`, …) are defined only for
> the Pi 4 build; the QEMU build leaves them undefined so the corresponding
> code is linked out or no-ops.

### 1.2 Boot Sequence (the unusual bits)

`loader/boot.S` does more than a textbook bare-metal stub:

1. **Mandatory Linux ARM64 Image header**
   - The Pi 4 EEPROM bootloader can refuse to jump into the kernel unless
     it sees the magic `"ARM\x64"` at file offset `0x38`.
   - `boot.S` places a 64-byte header (code0/code1/text_offset/image_size/
     flags/magic/res5) right at `_start:`.
   - **Symptom of missing header**: the firmware rainbow pattern stays on
     HDMI and your code never runs.
2. **Pin secondary cores with MPIDR_EL1**
   - The boot core (MPIDR_EL1 low 2 bits = 0) continues; cores 1/2/3 enter
     a `wfe` park loop until S0/S1 wakes them.
3. **Set up SP using the leex convention**
   - `sp = _start` (= `0x80000`) — kernel base address doubles as the
     initial stack pointer.
4. **Preserve the DTB pointer**
   - Firmware passes the device tree physical address in `x0`. The stub
     stashes it across BSS clear and writes it back to `.data` `dtb_addr`
     so `kernel_main` can find the simple-framebuffer node etc.
5. **Zero BSS, then jump to C**
   - 8 bytes per iteration from `__bss_start` for `__bss_size` words,
     then `bl kernel_main`. If `kernel_main` ever returns, drop into the
     same `wfe` park loop as the secondaries.

The stub does **not** implement an EL2→EL1 transition (same assumption
holds for QEMU). Adding `armstub=` or `kernel_old=1` would change this.

----------------------------------------------------------------------

## 2. What You Need

### 2.1 Hardware (for a real board)

- Raspberry Pi 4 (BCM2711)
- microSD card with a FAT32 `bootfs` partition
  - Easiest: flash Raspberry Pi OS first, then overwrite `kernel8.img`
    and `config.txt`
- 3.3 V USB-serial adapter, 115200 8N1
  - Header pin 8 (TXD → GPIO14), pin 10 (RXD → GPIO15), and a GND
    (e.g. pin 6)
- For networking: an Ethernet cable

### 2.2 Host software

- macOS or Linux
- AArch64 cross-toolchain (pick one):

```sh
brew install aarch64-elf-gcc            # GNU
brew install --cask gcc-arm-embedded    # ARM official
```

- `qemu-system-aarch64` for the QEMU target:

```sh
brew install qemu
```

- A serial terminal client (`screen`, `minicom`, `picocom`, …)

----------------------------------------------------------------------

## 3. Build

Always run `make` from `compile/` — the Makefile uses `VPATH` to gather
sources from `../loader/`, `../device/...`, etc.

### 3.1 Basic commands

```sh
cd /Users/kodamay/projects/xinu-rpi4/compile

make pi4            # → compile/kernel8.img       (real Pi 4)
make qemu           # → compile/kernel_virt.img   (QEMU virt)
make                # = make all = pi4 + qemu
make clean          # remove all objects and images
```

Objects live in per-variant trees (`obj/pi4/`, `obj/qemu/`),
so building different targets in sequence only recompiles what differs.

### 3.2 Toolchain auto-detection

The Makefile looks for AArch64 GCC in this order:

1. `$(GCCPATH)/bin/aarch64-elf-gcc` (Homebrew `aarch64-elf-gcc`)
2. `$(GCCPATH)/bin/aarch64-none-elf-gcc` (ARM official `gcc-arm-embedded`)

Default `GCCPATH` is `/opt/homebrew`. Override if installed elsewhere:

```sh
make pi4 GCCPATH=$HOME/aarch64/arm-gnu-toolchain-14.3.rel1-x86_64-aarch64-none-elf
```

You can also override the prefix:

```sh
make pi4 CROSS=aarch64-linux-gnu-
```

### 3.3 Key Make variables

| Variable     | Default                | Purpose                                |
|--------------|------------------------|----------------------------------------|
| `GCCPATH`    | `/opt/homebrew`        | Cross-toolchain install prefix         |
| `CROSS`      | (auto-detected)        | Tool prefix (`aarch64-elf-`, …)        |
| `SDCARD`     | `/Volumes`             | Parent mount point for `make install_*`|
| `DEST`       | `$(SDCARD)/bootfs`     | Target FAT32 partition                  |

### 3.4 Per-target macros (passed via CFLAGS)

| Macro         | Pi 4             | QEMU virt    |
|---------------|------------------|--------------|
| `-mcpu`       | `cortex-a72`     | `cortex-a76` |
| `UART0_BASE`  | `0xFE201000`     | `0x09000000` |
| `MBOX_BASE`   | `0xFE00B880`     | (undef)      |
| `SD_BASE`     | `0xFE340000`     | —            |
| `USB_BASE`    | `0xFE980000`     | —            |
| `GIC_BASE`    | `0xFF840000`     | —            |
| `PCIE_BASE`   | `0xFD500000`     | —            |
| `GENET_BASE`  | `0xFD580000`     | —            |
| `HEAP_END`    | `0x40000000` (1G)| `0x50000000` |
| `SKIP_MBOX`   | —                | defined (simplify) |
| `KERNEL_NAME` | `"kernel8.img"`  | `"kernel_virt.img"` |
| `BOARD_NAME`  | `"Pi4"`          | `"virt"`     |
| `SOC_NAME`    | `"BCM2711"`      | `"QEMU"`     |

Common CFLAGS:
`-Wall -O2 -ffreestanding -nostdinc -nostdlib -nostartfiles -mgeneral-regs-only`

`-mgeneral-regs-only` keeps most code FPU/NEON-free; only `cc.c` and
`llm.c` are built with FP enabled (and CPACR releases the FPU at EL1).

### 3.5 What gets linked in

`Makefile`'s `COMPONENTS` list:

```
loader mem system shell fs cc llm
device/uart device/mbox device/video device/sd device/usb
device/usb/xhci device/gic device/timer device/genet device/wifi
network/arp network/net network/netaddr network/ipv4 network/icmp
```

All `*.c` and `*.S` files in those directories are picked up with
`$(wildcard ...)`. Pi 4-only peripherals (GENET / SD / USB xHCI, …) are
real on the Pi 4 build (see §1.1); the QEMU build leaves their MMIO bases
undefined, so that code compiles to no-ops.

### 3.6 Linker scripts

| Script                     | Target    | Entry / load address |
|----------------------------|-----------|----------------------|
| `compile/link.ld`          | Pi 4      | `0x80000`            |
| `compile/link_virt.ld`     | QEMU virt | `0x40080000`         |

Both place `.text.boot` at the start and export `__bss_start` /
`__bss_size` (in 8-byte units), following the leex convention.

----------------------------------------------------------------------

## 4. Writing the SD Card

### 4.1 Pi 4

```sh
cd compile
make install_pi4 SDCARD=/Volumes
# → copies kernel8.img and sdcard/config_pi4.txt to /Volumes/bootfs
```

`make install` is an alias for `install_pi4`.

### 4.2 Manual copy (e.g. external SD reader)

```sh
diskutil mount /dev/disk4s1                # → /Volumes/bootfs
cp compile/kernel8.img /Volumes/bootfs/
cp sdcard/config_pi4.txt /Volumes/bootfs/config.txt
sync
diskutil eject /Volumes/bootfs
```

> ⚠ Verify the md5 of `kernel8.img` on the SD card matches what's in
> `compile/` — booting an old image is the most common time sink.

### 4.3 What else must be on the SD card

`kernel8.img` + `config.txt` alone won't boot. The same FAT32 partition
also needs the Raspberry Pi OS firmware blobs (`bootcode.bin`, `start4.elf`,
`fixup4.dat`, …). The fastest path is to flash Pi OS first, then overwrite
only `kernel8.img` and `config.txt`.

----------------------------------------------------------------------

## 5. Serial Console

Connect via the USB-serial adapter:

```sh
# macOS — screen
screen /dev/tty.usbserial-XXXX 115200

# macOS — minicom
minicom -b 115200 -o -D /dev/tty.usbserial-XXXX

# Linux
sudo screen /dev/ttyUSB0 115200
```

Exit `screen` with `Ctrl-a k` then `y`.

Within ~5 seconds of power-on you should see:

```
================================================
  Xinu Pi4 hello (AArch64, BCM2711, kernel8.img)
  PL011 UART0 @ 0xFE201000, 115200 8N1
  bootstrap: leex-style stub + xinu-rpi4 main
================================================

Round 1 phase B/U done — entering interactive shell.
type `help` for the command list.
xinu-pi4$ _
```

----------------------------------------------------------------------

## 6. Shell Command Reference

`help` or `?` always shows the live list. The current command set:

| Command | Purpose |
|---------|---------|
| `help` / `?` | List registered commands |
| `echo <words…>` | Echo args back (whitespace-collapsed) |
| `hello` | Smoke marker — greeting |
| `mem` | Show `__bss_start` / `__bss_end` / `_end` (from `link.ld`) |
| `peek <hex_addr>` | Read a 32-bit MMIO word (e.g. `peek 0xfe201018`) |
| `uptime` | Raw `CNTPCT_EL0` (generic timer counter) |
| `ticks` | 100 Hz timer tick counter (S1) |
| `ps` | Core / EL status |
| `halt` | Mask DAIF + PSCI `SYSTEM_OFF` (QEMU `virt` exits cleanly) |
| `reboot` | Stub — spins until power-cycle |
| `pwd` `ls` `cd` `mkdir` `rmdir` `touch` `cat` `write` `edit` `rm` `cp` `mv` `tree` | In-RAM filesystem (volatile; see §6.3) |
| `cc <file.c>` | Compile & run a C program on-device (JIT → AArch64; §6.3) |
| `aload` / `amsg` | Load resident AIPL actors / send a message (§6.3) |
| `actordemo` / `selectdemo` | Actor ping-pong / guarded named-message receive (§6.3) |
| `vmtest` / `vmdemand` | VA≠PA remap / demand-paged virtual memory (§6.4) |
| `llm [prompt]` | Generate text from the baked-in LLM (§6.3) |
| `preempt` | Demo the timer-driven preemptive round-robin scheduler |
| `pingpong [N]` | AIPL-style 2-actor cooperative PingPong, N=1..50 (default 5) |
| `procdemo [N]` | Real 2-process ctxsw demo, N=1..30 (default 5) |
| `usb` | xHCI / DWC2 USB diagnostics (Pi 4 only) |
| `wifi …` | WiFi + mesh — `probe`/`scan`/`up`/`adhoc`/`aodv`/… (§7.5–§7.6) |
| `rxstat` / `tcpstat` | RX-ring / TCP-listener counters |
| `pan <dx> <dy>` `view` `autopan [on|off]` | Window-manager viewport controls |

### 6.1 procdemo — real context switch

`procdemo 3` exercises actual AArch64 context switching:

```
xinu-pi4$ procdemo 3
procdemo: created pid=1 (ping) and pid=2 (pong), iters=3
---------------------------------------------
  [Ping pid=1] tick 1
  [Pong pid=2] tock 1
  [Ping pid=1] tick 2
  [Pong pid=2] tock 2
  [Ping pid=1] tick 3
  [Pong pid=2] tock 3
  [Ping pid=1] exit at iter 3
  [Pong pid=2] exit at iter 3
---------------------------------------------
procdemo: both processes exited; back in shell.
```

Each `pid=N` is read live from global `currpid`, so the alternation in
that column is direct evidence that the scheduler actually flipped
contexts. Stacks are not reclaimed on `proc_exit` yet — S1 (clock IRQ)
will let the dispatcher reap them.

### 6.2 pingpong vs procdemo

| Aspect            | `pingpong`                       | `procdemo`                              |
|-------------------|----------------------------------|-----------------------------------------|
| Actors            | Two static (`Ping`, `Pong`)      | Real procs created via `proc_create`    |
| Switch mechanism  | Single-stack dispatcher loop     | Real ctxsw via `ctxsw.S` (callee-saved) |
| Termination       | Both inboxes empty               | Both call `proc_exit()`                 |

### 6.3 On-device tooling (filesystem, C JIT, actors, LLM)

Beyond the demos above, the shell carries a self-contained toolchain — you can
write, compile, and run code on the board itself.

- **In-RAM filesystem** — `pwd` `ls` `cd` `mkdir` `rmdir` `touch` `cat` `write`
  `edit` `rm` `cp` `mv` `tree` operate on a small in-memory tree (volatile;
  cleared on reboot).
- **C JIT (`cc`)** — `cc <file.c>` compiles a C subset to native AArch64 and runs
  it in place. The same compiler is reachable over HTTP as `POST /compile`
  (body = C source).
- **AIPL actors** — `aload <file.c>` loads resident actors; `amsg <actor>
  <method> [arg]` sends a message. `actordemo` runs a 2-actor ping-pong as real
  Xinu processes; `selectdemo` shows a guarded receive (take a named message
  first). HTTP `/actor`, `/send`, and `/gc` expose the actor inventory, message
  send, and the actor-pool GC.
- **Embedded LLM (`llm`)** — a tiny transformer is baked into the image; `llm
  [prompt]` generates text on-device (also `/chat` over HTTP).

### 6.4 Virtual memory (`vmtest` / `vmdemand`)

The kernel runs with the MMU on (identity map) plus a **demand-paged window** at
VA `0x80000000`..`0x80400000` (4 MiB, 512-frame pool):

- `vmtest` — map a physical page at a different virtual address and prove the
  translation (VA ≠ PA).
- `vmdemand` — touch 64 pages in the demand window; the first run takes `0x40`
  page faults (one per page) and reads back OK, the second run takes `0x0` faults
  (already mapped). Check the fault counter with `/fault` (HTTP).

Both run locally or remotely, e.g. `curl
"http://192.168.3.100/shell?cmd=vmdemand"`.

----------------------------------------------------------------------

## 7. Networking (Pi 4)

The network stack runs on the Pi 4 GENET (BCM2711).

### 7.1 What works

- ARP request / reply
- ICMP echo (ping)
- Static IP / MAC: `192.168.3.100` / `d8:3a:dd:a7:fd:bf`
  (set in `loader/main.c:707` area)
- Raw broadcast / unicast TX
- 16-slot RX ring

DHCP and TCP code is in the tree but currently **dispatch-disabled** —
see §7.4.

### 7.2 Ping from a Mac

```sh
# After the Pi 4 has booted:
sudo arp -s 192.168.3.100 d8:3a:dd:a7:fd:bf
ping 192.168.3.100
```

RTT is ~900 ms (rate-limited by the window-manager tick).

### 7.3 Quick checks from the shell

```
xinu-pi4$ rxstat
xinu-pi4$ ticks
```

### 7.4 DHCP / TCP status

`system/dhcp_client.c` (DHCP client) and `system/tcp_server.c`
(single-connection TCP listener) ship in the tree but their `rx_tick`
dispatch is **OFF**:

- **DHCP**: `dhcp_send_discover()` puts the right frame on the wire,
  but many home routers' WiFi↔LAN bridges drop DHCP broadcasts
  one-way; in addition, repeated broadcast TX degrades the GENET TX
  ring and stalls ICMP. Boot-time DISCOVER and the 5 s retry loop are
  disabled in `main.c`.
- **TCP**: SYN+ACK / echo / FIN are implemented and SYNs on port 23
  reach the receiver, but calling `tcp_handle_packet()` from `rx_tick`
  causes subsequent ICMP echo replies to stop (under investigation in
  `NEXT_SESSION.md`).

So the code exists but the default boot has it sleeping. Re-enabling is
planned in a future sprint (NET-G bisect).

### 7.5 WiFi (BCM43455)

The Pi 4 has an on-board BCM43455 WiFi chip (separate from the wired GENET of
§7.1). It is driven entirely from the serial shell (`xinu-pi4$`): bring up the
firmware, scan, then connect. It does **not** auto-connect at boot.

| Command | What it does |
| --- | --- |
| `wifi probe` | M0/M1 bring-up: download the firmware to the chip (run once per boot) |
| `wifi scan` | escan for nearby APs |
| `wifi up <ssid> <pass>` | join (WPA2-PSK) + DHCP; starts the persistent ARP/ICMP responder |
| `wifi join <ssid> <pass>` | join only (no DHCP) |
| `wifi dhcp` | request a DHCP lease |
| `wifi off` | radio down / disconnect |
| `wifi ping <a.b.c.d> [count]` | ICMP echo client |
| `wifi serve [secs]` | run the ARP/ICMP responder for N seconds (default 30) |
| `wifi resolve <host>` / `wifi web <host>` | DNS / DNS + HTTP GET |
| `wifi ntp [a.b.c.d]` | NTP time client |
| `wifi tftp <ip> <file>` / `wifi netboot <ip> <file>` | TFTP fetch / fetch + chainload |

Typical connect:

```
xinu-pi4$ wifi probe          # download firmware (once per boot)
xinu-pi4$ wifi scan           # see what's around
xinu-pi4$ wifi up MyHome-5G mypassword
wifi up: connected; ARP/ICMP responder is now persistent.
xinu-pi4$ wifi ping 8.8.8.8
```

> WiFi does not auto-reconnect after a reboot — re-run `wifi probe` + `wifi up`
> each boot. `wifi netboot <ip> <file>` fetches a kernel over WiFi and chainloads
> it (a no-SD-swap update path, like the wired chainload).

### 7.6 Mesh networking with multiple Xinu nodes (MANET ad-hoc)

Several Pi 4 (and Pi 3) Xinu boards can form a peer-to-peer mesh with **no access
point**, using 802.11 IBSS (ad-hoc) mode plus on-demand AODV routing — the same
MANET stack used in the drone-HIL demo (Pi 4 + Pi 3 nodes).

Each node joins the same ad-hoc cell with a distinct node number:

```
xinu-pi4$ wifi probe                       # once per boot
xinu-pi4$ wifi adhoc <cell-ssid> [ch] [node]
```

- `<cell-ssid>` — the cell name; **all nodes must use the same name and channel**.
- `[ch]` — 802.11 channel (default 6).
- `[node]` — this node's number (default 1); it gets the static IP `10.0.0.<node>/24`. **Give every node a distinct number.**

**Example — a 3-node cell on channel 6:**

```
xinu-pi4$ wifi adhoc mesh1 6 1      # -> 10.0.0.1
xinu-pi4$ wifi adhoc mesh1 6 2      # -> 10.0.0.2
xinu-pi4$ wifi adhoc mesh1 6 3      # -> 10.0.0.3
```

Nodes within radio range reach each other directly at `10.0.0.x`
(`wifi ping 10.0.0.2`).

**Multi-hop (AODV).** When a destination is not in direct range, discover a route
on demand with `wifi aodv <ip>`:

```
xinu-pi4$ wifi aodv 10.0.0.3
[wifi] === AODV discover 10.0.0.3 ===
[wifi] aodv: RREQ id=1 for 10.0.0.3
[wifi] *** AODV route: 10.0.0.3 via 10.0.0.2, 2 hop ***
```

The minimal AODV module (M13, RFC 3561 core) broadcasts an RREQ (UDP port 654);
each in-range node relays it and records a reverse route; the destination answers
with an RREP that installs the forward route. Every node relays for its peers
automatically, so intermediate nodes forward traffic beyond direct range (route
table: up to 16 entries).

> IBSS / ad-hoc is independent of the infrastructure `wifi up` mode — run
> `wifi off` to leave an AP first. Ad-hoc is not restored after reboot; re-run
> `wifi probe` + `wifi adhoc` on each node.

### 7.7 HTTP control plane & remote shell

`system/tcp_server.c` runs an HTTP server on the wired interface (default port
80, `192.168.3.100`), so the board can be driven entirely over the network:

| Route | What it does |
| --- | --- |
| `GET /shell?cmd=<cmd>` | run any shell command, return its output (commands have no stdin) |
| `POST /compile` | JIT-compile and run a C program (body = source) |
| `GET /chat` | on-device LLM chat |
| `GET /actor` `/send` `/gc` `/jitstats` | actor inventory / message send / actor-pool GC / JIT counters |
| `GET /usbdiag` `/pcie-init` `/pcie-enum` `/xhci-reset` | USB / PCIe bring-up diagnostics |
| `GET /fault` `/mmio-read` `/mmio-write` `/mmio-sweep` | fault counters + raw MMIO peek/poke |
| `POST /reboot` | BCM2711 watchdog reset |
| `POST /chainload` | upload + jump to a new kernel (no SD swap) |

Runtime diagnostics use these HTTP counters because `uart_puts` deadlocks in the
HTTP-worker context — all PCIe/xHCI bring-up logging is done at boot over serial.

**Network kernel update (chainload — no SD swap).** `POST /chainload` uploads a
new kernel and jumps to it in RAM. A helper script wraps the upload:

```sh
python3 tools/remote_chainload.py 192.168.3.100 compile/kernel8.img
```

A bad image just needs a power cycle (the SD card is untouched), which keeps the
dev loop fast. It needs the HTTP server to be responsive to accept the upload.

----------------------------------------------------------------------

## 8. Running under QEMU

Even without hardware, you can reach the shell:

```sh
cd compile
make qemu          # interactive — Ctrl-A X to quit
make qemu-smoke    # canned input, writes qemu-smoke.log
```

Under QEMU:

- MIDR_EL1 = `0x414fd0b1` (QEMU's published Cortex-A76 part number; real
  Pi 4 hardware runs Cortex-A72)
- `halt` cleanly exits because `virt` handles PSCI SYSTEM_OFF
- No networking, USB, SD, or HDMI (hardware not modelled)

----------------------------------------------------------------------

## 9. Important config.txt Lines

`sdcard/config_pi4.txt` (copied to the card's `config.txt` by
`make install_pi4`):

| Line                            | Meaning                                          |
|---------------------------------|--------------------------------------------------|
| `arm_64bit=1`                   | Boot in AArch64                                  |
| `kernel=kernel8.img`            | Pi 4 kernel name                                 |
| `kernel_address=0x80000`        | Load address                                     |
| `enable_uart=1`                 | Enable UART                                      |
| `uart_2ndstage=1`               | Print second-stage bootloader log on UART        |
| `dtparam=uart0=on`              | Route UART0 to GPIO14/15                         |
| `init_uart_clock=48000000`      | Pin the PL011 reference clock to 48 MHz (115200) |
| `hdmi_force_hotplug=1`          | Force HDMI hotplug                               |
| `framebuffer_width=640`         | Width of firmware-allocated framebuffer          |
| `framebuffer_height=480`        | Height                                           |
| `framebuffer_depth=32`          | bpp                                              |

The KMS overlay (`dtoverlay=vc4-kms-v3d`) is left in — Xinu still
consumes the firmware's simple-framebuffer via the VC mailbox.

----------------------------------------------------------------------

## 10. USB / Input Device Support

The Pi 4's USB-A ports go through **VL805 (PCIe xHCI)**; a hand-rolled
xHCI driver brings up USB keyboards and mice.

Key points and constraints:

- **Use the USB-2.0 (black) ports.** They route through the USB-2.0 hub
  (Genesys, with a working TT), so low-speed interrupt-IN split
  transactions are delivered. The USB-3.0 (blue) ports sit behind the
  VIA USB3 hub whose path does not deliver the periodic transfer, so
  the cursor won't move.
- The shell's `usb` command is a **diagnostic dump** for the DWC2 (USB-C
  OTG) HCD, not a general device-control interface.
- The Pi 4 USB-C port is **DWC2 OTG**; the USPi (`extern/uspi/lib`)
  driver is currently disabled.
- VL805 firmware leaves the PCIe RC in reset on an SD boot, so boot issues
  a `NOTIFY_XHCI_RESET` mailbox tag (`0x00030058`, devid `0x100000`) to
  bring xHCI up (details in `NEXT_SESSION_PI4.md`).

So shell I/O is:

| Board    | Input                       | Output                                |
|----------|-----------------------------|---------------------------------------|
| Pi 4     | USB-A keyboard / UART0 serial | UART0 serial + HDMI framebuffer       |
| QEMU     | qemu stdio (serial)         | qemu stdio (serial)                   |

To drive the board over serial you still need a USB-serial adapter plus
`screen`/`minicom` on the host.

----------------------------------------------------------------------

## 11. Troubleshooting

| Symptom                                  | What to check                                                       |
|------------------------------------------|---------------------------------------------------------------------|
| No output on serial                      | (1) device name + speed in `screen`, (2) TX/RX swapped, (3) GND     |
| Banner appears, then stops               | Old `kernel8.img` on SD — rebuild and recopy                        |
| No echo at the prompt                    | Local echo on the host terminal — keep it OFF (`screen` default)    |
| `peek` returns 0xFFFFFFFF                | Address not mapped. MMU is flat ID — stay within real regions       |
| USB keyboard / mouse does nothing        | Make sure it's on a USB-**2.0 (black)** port (§10)                  |
| Ping fails on Pi 4                       | (1) `sudo arp -s ...` done?, (2) cable, (3) `rxstat` increments?    |
| `make qemu` fails                        | Check `qemu-system-aarch64` version (≥ 11 recommended)              |
| `make install` fails with permission err | It uses `sudo cp` internally — provide the sudo password            |
| Reboot loop on power-on                  | Does `kernel=` in `config.txt` match the actual file on the card?   |
| Rainbow pattern stays on HDMI            | Likely missing Linux ARM64 Image header — rebuild with current src  |

Generic recovery flow:

1. `cd compile && make clean && make pi4`
2. `make install_pi4 SDCARD=/Volumes`
3. `diskutil eject /Volumes/bootfs`
4. Reseat the SD card and power-cycle the board.

----------------------------------------------------------------------

## 12. Directory Layout (operator view)

The places you actually touch:

```
xinu-rpi4/
├── compile/                # run `make` here
│   ├── Makefile
│   ├── kernel8.img         # Pi 4 (built by `make pi4`)
│   └── kernel_virt.img     # QEMU (built by `make qemu`)
├── sdcard/
│   └── config_pi4.txt      # Pi 4 (copied by install_pi4)
├── README.md               # Developer-facing roadmap + internals
├── USERS_MANUAL_EN.md      # This file
├── USERS_MANUAL_JA.md      # Japanese version
└── NEXT_SESSION_PI4.md     # Handoff notes for ongoing work
```

For source layout, see the README's Layout section.

----------------------------------------------------------------------

## 13. Recipe Book

### From a fresh checkout to a running Pi 4

```sh
cd /Users/kodamay/projects/xinu-rpi4/compile
make clean
make pi4
make install_pi4 SDCARD=/Volumes
diskutil eject /Volumes/bootfs
# Reseat the SD card in the Pi 4 and power it on.
screen /dev/tty.usbserial-XXXX 115200
```

### Pi 4 ping check

```sh
cd /Users/kodamay/projects/xinu-rpi4/compile
make pi4
make install_pi4 SDCARD=/Volumes
diskutil eject /Volumes/bootfs
# Power on the Pi 4.

# On the host:
sudo arp -s 192.168.3.100 d8:3a:dd:a7:fd:bf
ping 192.168.3.100
```

### Quick command sanity check (QEMU)

```sh
cd /Users/kodamay/projects/xinu-rpi4/compile
make qemu
# At the prompt:
xinu-pi4$ procdemo 5
xinu-pi4$ pingpong 3
xinu-pi4$ halt
# Ctrl-A X to exit QEMU
```

----------------------------------------------------------------------

## 14. References

- Upstream Xinu (32-bit): https://github.com/yaskodama/xinu-rpi
- AArch64 boot stub origin: https://github.com/radlyeel/leex
- Shell centry pattern origin: https://github.com/davidxyz/xinuPi
- Development roadmap (Round 1 plan):
  under `aice-pi-evolution/experiments/` in the abclcp-project repo

----------------------------------------------------------------------

## 15. License

Inherits the BSD-style licenses of upstream Xinu and leex. See `LICENSE`
once the source-of-truth file is added.
