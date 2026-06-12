# BCM2711 firmware PCIe gate — disassembly + MMIO confirmation

Investigation arc into why xHCI / USB-A never comes up on bare-metal
Pi 4 even after replaying the CPRMAN PCIe-clock sequence extracted from
`start4.elf` disassembly.

## TL;DR

`start4.elf` firmware contains a single 1-bit "PCIe present" gate at
**`0xFE0000B4` bit 0** (VC4-view `0x7E0000B4`).  On bare-metal Pi 4 boot
this bit is **0**, causing firmware to skip the entire CPRMAN→PCIe
init block.  The gate is writable from ARM via direct MMIO and the
write sticks — but flipping it post-boot is necessary, not sufficient:
firmware's PCIe init also touches VC4-internal state at `gp+0x3c0d0`
(bits 24, 25, 26) which is not addressable from ARM.

## Evidence chain

### 1. Disassembly trail (`references/start4.S`)

* `ec63f00` is the board-dispatch function with a 37-case switch on
  `version r4 - 0x4000160`.  Active cases (r4 ∈ {0,1,2,16,32,33,34,35,36})
  set a cascading 4-flag chain ending at `st r9,(gp+5476)` (the PCIe
  flag).  Other 28 cases jump to default = no flag set.
* `ec63fec..ec63ff6`: regardless of which case fires, firmware
  subsequently reads `*(0x7E000080 + 0x34) = *(0x7E0000B4)` and
  branches away (`beq 0xec64052`) if bit 0 is clear, skipping the
  remaining CPRMAN/PCIe init.

### 2. MMIO confirmation (recorded on Pi 4 8GB rev d03115)

```
/mmio-read?addr=0xFE0000B4 → val=0x0 bit0=0       (gate is OFF)
/mmio-sweep?addr=0xFE000080&n=16 → mostly 0/1 flags + "MULT" sentinel
/mmio-write?addr=0xFE0000B4&val=0x1 → before=0 after=1 [STUCK]   (writable!)
/mmio-read?addr=0xFEE01000 (post-gate-force) → val=0 (was wedge before)
```

### 3. Necessary-but-not-sufficient

After gate force + `/cprman-init` (write `0x5A000016` → `CPRMAN+0x128`):

```
EMMC2CTL [+0x1d0] = 0x296   (bit 7 BUSY=1 — clock running, baseline good)
PCIE?CTL [+0x128] = 0x016   (bit 7 BUSY=0 — clock NOT running)
```

The CTL register accepts the SRC=6/ENAB write but the gate hardware
doesn't activate.  The full firmware sequence (`ec63fa8` onwards)
touches `gp+0x3c0d0` bits 24, 25, 26 between the gate check and the
CPRMAN write — those are VC4-internal addresses not reachable from
ARM, blocking us from completing the init sequence.

## Memory map findings (Pi 4 8GB)

| Range | Content | Notes |
|-------|---------|-------|
| 0xFE000000-7C | all 0 | reserved / header |
| 0xFE000080-BF | mixed flags + 0x4D554C54 ("MULT") @ 0x8C + 0x0A060000 @ 0xBC | firmware feature-config region |
| **0xFE0000B4** | **0** ← PCIe gate | bit 0 = "PCIe present" |
| 0xFE0000C0-C8 | 0/1/1 | adjacent feature group |
| 0xFE0000CC+ | "MULT" sentinel | unused entries |
| 0xFE102390 (CPRMAN+0x1390) | 0x300021 | accepts password write (sets bit 21, clears bit 24) |

## New HTTP routes added this arc

| Route | Purpose |
|-------|---------|
| `GET /mmio-read?addr=0xN` | Direct MMIO read via setjmp-fault-catch |
| `POST /mmio-write?addr=0xN&val=0xV` | Direct MMIO write + read-back STUCK check |
| `GET /mmio-sweep?addr=0xN&step=4&n=16` | Range sweep, fault tolerant |
| `GET /pcie-fw-probe` | Mailbox proxy probe (proven safe addresses) |
| `GET /pcie-fw-probe1?addr=0xN` | Single-address mailbox probe |
| `POST /pcie-fw-gate-force` | Mailbox-attempt to set the gate (turned out direct MMIO works better) |

## Dead-end signals (so future sessions don't repeat)

* `GET_PERIPH_REG` mailbox tag (`0x00030045`) returns `value=0` with
  `tag-resp=0x80000002` (a 2-byte error response, not the 4-byte
  HANDLED) for CPRMAN addresses on our firmware.  Mailbox-proxied
  reads are NOT a viable channel for probing peripheral state on this
  firmware version.  `GET_FIRMWARE_REVISION` works (tag-resp=0x80000004
  with a real value), so the mailbox plumbing itself is fine.
* `/pcie-clk-full` (the 11-step extracted sequence) WEDGES Pi 4 —
  per-step bisection (see below) showed **Step 7 (CPRMAN+0x108 =
  0x5A0003C0)** is the wedger.  By Linux clk-bcm2835.c convention +0x108
  is PLLC routing, and writing 0x3C0 there appears to break the
  ARM-core clock path.  All other 10 steps individually do NOT wedge.
* `0xFEE01000` reads as 0 but writes don't stick (probably read-only
  status, not a power-domain control register).

## Per-step bisect result (2026-05-31, via `/mmio-write`)

After gate force unblocks bus access, each of the 11 firmware-extracted
steps was issued as an individual `/mmio-write`:

| Step | Addr | Value | Result |
|------|------|-------|--------|
| 1 | 0xFEE01000 | 0x1 | NOT-STUCK (read-only status) |
| 2 | CPRMAN+0x128 | 0x5A000036 | OK (KILL+ENAB+SRC=6) |
| 3 | CPRMAN+0x12C | 0x5A000000 | OK (DIV=0) |
| 4 | CPRMAN+0x1390 | 0x5A200001 | OK (bit 21 set, bit 24 cleared) |
| 5 | 0xFEC11010 | 0x0 | NOT-STUCK (already 0) |
| 6 | 0xFEC11014 | 0x0 | NOT-STUCK (same) |
| 8 | CPRMAN+0x20 | 0x5A000040 | OK (no-op, was 0x40) |
| 9 | CPRMAN+0x34 | 0x5A000000 | OK (0x2000 → 0) |
| 10 | CPRMAN+0x30 | 0x5A000040 | OK (0x244 → 0x240) |
| 11 | CPRMAN+0x30 | 0x5A000011 | **CPRMAN+0x30 BUSY=1** (0x240 → 0x291) |
| **7** | **CPRMAN+0x108** | **0x5A0003C0** | **💀 WEDGE** (0 → 0x380) |

Even skipping Step 7 entirely, CPRMAN+0x128 (PCIE?CTL) BUSY never sets.
Step 7 isn't the missing fix either.

## The deeper realization: +0x128 isn't a clock-gate

A wider CPRMAN sweep (0xFE101000..0xFE1011FC + 0xFE102100..0xFE10217C)
showed peripheral clocks happily running with SRC=6 (PLLD):

* EMMC2 (+0x1D0) = 0x296 → BUSY=1, SRC=6 PLLD — canonical active clock
* +0x1C0..+0x1F8 contain many BUSY=1 clocks on SRC=6 / SRC=1
* A2W PLL config at +0x1100/+0x1120/+0x1140 reads 0x21037/0x21030/
  0x21037 — PLLA/C/D are live and configured

So SRC=6 with PLLD as source DOES work as a clock-gate config on this
firmware.  Why does CPRMAN+0x128 BUSY never set?

Cross-referencing Linux clk-bcm2835.c:

| Offset | Linux name | Real use |
|--------|------------|----------|
| +0x108 | PLLC_CTRL | PLLC routing mux (Step 7 wedger) |
| +0x128 | **CM_CKSM** | clock checksum/status — *NOT a peripheral clock-gate* |
| +0x12C | CM_OSCFREQI | OSC frequency capture |
| +0x1D0 | EMMC2 CTL (BCM2711 ext) | real clock gate — BUSY works |

The firmware code at `ed4995e` writes to CPRMAN+0x128 during PCIe init,
but **+0x128 is CM_CKSM, not a clock gate**.  The firmware write to it
is bookkeeping — probably the status register that the gate at
0x7E0000B4 mirrors.  Our gate + 11-step sequence has been replaying
firmware's *announcement* of PCIe presence, not its *clock-on*
trigger.

The real PCIe clock-enable register is elsewhere — probably in PM
(0xFE100000+), in another clock controller block, or done entirely
through VC4-internal state (`gp+0x3c0d0` bits 24/25/26 we noted
earlier).  Without finding the actual clock-on register, ARM-side
replay of any extracted firmware sequence can't bring up PCIe.

USB-A via VL805 stays out of reach — but the diagnostic toolkit
(`/mmio-read`, `/mmio-write`, `/mmio-sweep`) is now solid enough to
resume the hunt without further flash cycles for read-only probing.

## What would actually fix it (superseded — see next section)

This earlier guess (continue disasm past `ec64052`, bisect
`/pcie-clk-full`) was based on the assumption that the 11-step
sequence and the `gp+5476`/`0xFE0000B4` gate were the actual PCIe
clock-on path.  Subsequent disassembly (2026-05-31 part 2, below)
showed that's wrong: `ec36980` and friends are *status reporting*
phase, not the real init.  The real init runs from a different
function (`ec3d828`, the `_icp` phase) and is itself gated by a
firmware-private TLV descriptor.  Bisecting the 11-step sequence
will never set BUSY=1 because those 11 steps aren't the clock-on
sequence.

## The real PCIe init path (2026-05-31 part 2)

Tracing pcie_init's callers led to the firmware boot-phase
dispatcher at `ec2cb90`.  It writes 4-byte ASCII tags to a debug
trace register (`0xCEC02000`) before calling each phase, giving
a clean map of the boot order:

| Phase tag (LE bytes) | Function | Comment |
|----------------------|----------|---------|
| `_msh` | `ec2fccc` | unknown |
| `_osh` | `ec36bd4` | unknown |
| `_ush` | `ec2fcd0` | unknown |
| **`_icp`** | **`ec3d828`** | **★ real PCIe / device init ★** |
| `_mih` | `ec2fcd2` | unknown |
| `_oih` | `ec36980` | status report — sets gate flag (what we explored above) |
| `_uih` | `ec2fcd6` | USB init |
| `_ini` | `edd8570` | finalize |

So the gate flag at `0xFE0000B4` and the cascade at `ec63f00`
that sets `gp+5476` aren't *causing* PCIe init — they're
*announcing* that init already happened, written during the
later `_oih` status phase.  PCIe init itself runs in `_icp` via
`ec3d828`, before `_oih`.

### What `ec3d828` actually does

It's not a "do PCIe init" function at all — it's a **generic TLV
(tag-length-value) descriptor dispatcher** for the `_icp` boot
phase.  Reverse-engineered shape:

```c
void icp_phase(void) {
    char *marker = gp + 0xfffd4110;          // VC4-internal SRAM
    if (memcmp(marker, "INI", 3) != 0) return;
    if (marker[3] > 3)                       return;

    void  *list;
    handler_t handler;
    if (marker[3] >= 3) {
        list    = gp + 0xfffd4118;           // new format
        handler = *(handler_t*)(marker + 4); // custom handler from marker
    } else {
        list    = gp + 0xfffd4114;           // old format
        handler = default_handler;           // ec3d8e0 — calls ed219be
    }

    while (1) {
        uint32_t tag  = list[0];
        uint32_t size = list[1];
        if (tag == 0 && size == 0) break;    // end of list

        void *data = list + 8;
        if (size > 0) {
            handler(data, size, ?, sp);      // dispatch one TLV entry
        } else {
            ed21b24(tag, 0, -size);          // destroy/free
        }
        list = align4(data + size);
    }
}
```

Each TLV entry in the descriptor specifies a device/feature to
initialize.  PCIe is one entry; presumably USB-HCD, GENET, MMC2
etc. are others.

### Why bare-metal can't reach this

Three nested requirements, each unreachable from ARM:

1. **The `INI` marker** lives at `gp - 0x2BEF0` in firmware-private
   VC4 SRAM.  VC4 GP-relative base is somewhere in the
   `0xF0288xxx` range (seen in several `mov gp, 0xf02xxxxx`
   instructions).  By the BCM2711 VC4 memory map, `0xF0xxxxxx`
   is VC4 internal SRAM, not aliased into ARM's view.  We probed
   the corresponding cached-SDRAM alias `0x3025BD90..` — reads
   succeed but the area is plain DRAM (0xFFFFFFFF), not firmware
   data.  Confirms VC4 GP-space is unreachable from ARM via
   simple aliasing.
2. **The TLV list** behind the marker is itself in the same
   VC4-private region.
3. **Constructing a valid TLV** would also need ARM to know every
   tag's format — that information lives only in the individual
   handler functions inside start4.elf, each tens-to-hundreds of
   instructions, none of which we've mapped.

The marker is presumably written by an earlier boot stage
(recovery.bin or start.elf) as a handshake telling start4.elf
"I'm a trusted caller, here's the device list to init."  Bare-metal
kernels load directly as `kernel8.img` and skip the handshake,
so the marker is absent and every TLV-driven init silently
no-ops.  All three of our observed symptoms — `0xFE0000B4 bit 0
== 0`, `CPRMAN+0x128` never reaches BUSY=1, MMIO at `0xFD500000`
wedges the AXI bus — are downstream consequences of `_icp` doing
nothing.

### Conclusion

USB-A via VL805/xHCI on bare-metal Pi 4 is **structurally
unreachable** without either reproducing the recovery.bin →
start.elf chain or modifying start4.elf itself.  Both are out
of scope for an Embedded Xinu kernel.

For USB input the actionable paths are:
* **DWC2 host on USB-C** (the SoC-internal controller, not
  PCIe-gated).  Verified alive on this hardware: GSNPSID =
  0x4F54_280A, GUSBCFG bit 29 (ForceHstMode) = 1, GHWCFG2
  reports 8 host channels.  Requires writing a DWC2 host
  driver + USB enumeration + HID class — large but feasible
  (~1000 lines for a minimal mouse driver).
* **Network HID** (already done — `/type` + `/click`) for
  immediate input without any USB driver work.

The diagnostic toolkit built during this arc (`/mmio-read`,
`/mmio-write`, `/mmio-sweep`, `/pcie-fw-probe[1]`,
`/pcie-fw-gate-force`) remains useful for any future bare-metal
peripheral hunt on Pi 4.
