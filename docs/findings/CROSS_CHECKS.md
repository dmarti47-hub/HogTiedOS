# Cross-checks against public sources

Findings from checking the handoff files against upstream U-Boot and Linux
source. Nothing here was tested on hardware either. The original JSON files are
left unmodified; this file records where they are corroborated, contradicted, or
corrected.

Sources used: U-Boot `master` (`arch/arm/mach-omap2/omap3/emif4.c`,
`arch/arm/include/asm/arch-omap3/{cpu.h,emif4.h}`), Linux `master` / v6.18.54
(`arch/arm/mach-omap2/{io.c,id.c,sdrc.c,pdata-quirks.c}`,
`arch/arm/boot/dts/ti/omap/{omap3,omap34xx,am3517}.dtsi`).

## 1. Memory controller at 0x6D000000 is very likely TI EMIF4 (AM35xx)

**Confidence: high on the identification, from structural match. Not
confirmed by any TI document for DRA52x.**

The IPL's DDR sequence (`boot_path_map.json` → `ddr_controller_ipl_config`)
matches U-Boot's AM35xx `do_emif4_init()` step for step, and every offset it
writes is a field of U-Boot's `emif4_t`:

| Offset | EMIF4 field (U-Boot)        | IPL value      | U-Boot AM3517 EVM value | Match |
|--------|-----------------------------|----------------|-------------------------|-------|
| 0xE4/E8| DDR_PHYCTRL1 / _SHDW        | 0x8006         | 0x6                     | differs (bit 15) |
| 0xEC   | DDR_PHYCTRL2                | 0x0            | 0x0                     | exact |
| 0x60   | SDRAM_IODFT_TLGC (bit10 = PHY reset, bit0 after) | RMW | RMW         | same sequence |
| 0x04   | SDRAM_STS (bit2 = PHY ready)| poll bit 2     | poll bit 2              | same |
| 0x18/1C| SDRAM_TIM_1 / _SHDW         | 0x06803292     | 0x06668292              | board-specific |
| 0x20/24| SDRAM_TIM_2 / _SHDW         | 0x201C320A     | 0x201C320A              | **exact** |
| 0x28/2C| SDRAM_TIM_3 / _SHDW         | 0x257          | 0x257                   | **exact** |
| 0x38/3C| PWR_MGMT_CTRL / _SHDW       | 0x80000000     | 0x80000000              | **exact** |
| 0x10/14| SDRAM_REF_CTRL / _SHDW      | 0x50F          | 0x50F                   | **exact** |
| 0x08   | SDRAM_CONFIG                | 0x40801632 / 0x408016B2 | 0x40801432     | differs only in ROWSIZE [9:7] |

This explains the "CS0/CS1 pairs 4 bytes apart" oddity: those are EMIF4
register/shadow-register pairs, not CS0/CS1.

Corrections to `boot_path_map.json`:
- The 0x6d000020 composite is `2|8|0x3200|0x1c0000|0x20000000` = **0x201C320A**,
  not 0x201c3200 as written.
- The IPL is described as polling 0x6d000060 bit 0x400 "until set"; U-Boot polls
  until it *clears*. Worth re-checking the disassembly branch sense. It doesn't
  affect us, since we never run this code.

**DRAM size (corrected, see sec. 7).** An earlier version of this file said
ROWSIZE couldn't be turned into a size. That was wrong: stock QNX startup uses
exactly this field. ROWSIZE 5 means 512 MiB, anything else 256 MiB.

**The "never write DDR/SDRC init" rule is unchanged.** This only tells us what
the silicon we're avoiding probably is.

## 2. Mainline Linux writes to 0x6D000000 during boot (hazard)

**Confidence: confirmed by reading kernel source.**

For any DT whose root is compatible with `ti,omap3`, `pdata_quirks_init()` calls
`omap_sdrc_init(NULL, NULL)` → `omap2_sdrc_init()`, which:

- RMWs `0x6C000010` (SMS_SYSCONFIG on OMAP3)
- RMWs `0x6D000010` (SDRC_SYSCONFIG on OMAP3; **SDRAM_REF_CTRL on EMIF4**):
  clears bits 4:3 and sets bit 4, turning refresh 0x50F into 0x517
- writes `0x6D000070` (SDRC_POWER on OMAP3; a reserved hole on EMIF4)

It then calls `_omap2_init_reprogram_sdrc()` (a DPLL3 M2 set_rate with the
current rate). The omap3 PM/idle code also touches SDRC_POWER in
`omap_sram_idle()`.

Mitigation in this repo: a kernel patch skips `omap_sdrc_init()` for
`hogtied,boombox-6.5gt`, and the kernel config fragment disables CONFIG_PM,
CONFIG_CPU_IDLE and CONFIG_CPU_FREQ.

## 3. SoC base: AM35xx vs OMAP3530

EMIF4 is an AM35xx trait (OMAP3530 has SDRC), and `boot_path_map.json` already
notes "the AM35xx CM/PRM map used elsewhere". The QNX driver names
(`omap3530.conf`, `devg-omap35xx`, `-domap3530`) point the other way. That's
weak evidence, though, since AM35xx BSPs reuse the OMAP35xx drivers.

**Decision: base the DTS on `am3517.dtsi`, marked tentative.** The kernel
prints the CONTROL_IDCODE hawkeye on boot (`id.c`); AM35xx is `0xb868`. The
first UART boot log settles this. A wrong base means wrong clock setup
(peripherals don't come up), not a brick risk.

## 4. Address and IRQ cross-check vs mainline `omap3.dtsi`

Every MMIO base and IRQ in the findings matches mainline:

| Block  | Findings           | Mainline             |
|--------|--------------------|----------------------|
| UART1  | 0x4806A000 IRQ 72  | 0x4806a000, 72       |
| UART2  | 0x4806C000 IRQ 73  | 0x4806c000, 73       |
| UART3  | 0x49020000 IRQ 74  | 0x49020000, 74       |
| McSPI3 | 0x480B8000 IRQ 91  | 0x480b8000, 91       |
| MMC3   | 0x480AD000 IRQ 94  | 0x480ad000, 94       |
| MMC2   | 0x480B4000 IRQ 86  | 0x480b4000, 86       |
| DSS    | 0x48050000 IRQ 25  | 0x48050000, 25       |
| DSI    | 0x4804FC00         | 0x4804fc00           |
| GPMC   | 0x6E000000         | 0x6e000000           |
| INTC   | 0x48200000         | 0x48200000           |
| GPIO1/5/6 | 0x48310000 / 0x49056000 / 0x49058000 | same |

**Contradiction:** `display_graphics_map.json` says 0x5D000000 "is the
documented, correct OMAP3530 SGX530 register base". In mainline OMAP3 DTs,
0x5D000000 is the **IVA2 MMU** (`mmu_iva`), and SGX is at **0x50000000**. What
devg-omap35xx maps there is unknown. This doesn't matter to us because we
aren't using SGX.

McSPI2 (the DSP bus) has no base address in the findings. Mainline puts it at
0x4809a000, IRQ 66, and the DTS uses that as mainline-derived.

## 5. Other corrections and inconsistencies

- **GPIO bitmask arithmetic** (`dsp_layout_map.json` → `dsp_init_0x126a08`):
  `0x2e008` is bits 3, 13, 14, 15, 17, i.e. **GPIO99, 109, 110, 111, 113**, not
  "3,13,15,16,17". GPIO112 (GPS reset) is therefore *not* touched by DSP init.
  This agrees with the pwramp code's `0xE000` (bits 13–15 = GPIO109–111).
- **IPC handshake IRQs 136/137** (`ioc_link_map.json`) exceed the OMAP3 INTC's
  96 lines, so they are QNX-remapped numbers (probably GPIO interrupts). **The
  GPIO mapping is unknown**, and it blocks a front/IOC driver.
- **Boot-mode channel:** `ioc_link_map.json` says ioc-boot-mode reads "ch8", but
  dev-ipc runs with `-c8`, which gives ch0–ch7. `boot_path_map.json` says boot
  mode comes over `/dev/i2c0`. These can't all be right as written. Unresolved.
- **QNX I2C numbering:** resolved in sec. 7. `/dev/i2c2` is OMAP I2C3.
- **EEPROM size lower bound:** the `touchCal` field at 0x1874 + 28 bytes means
  the part is at least 8 KiB (≥ 24C64-class). Exact part unknown.

## 6. Open items that gate the image packer (not yet in any findings file)

- Where the IPL copies the image in RAM (whole `stored_size` to `image_paddr`?),
  and the CPU state/registers at the jump to `startup_vaddr` (MMU and caches
  off? r0–r2?). A Linux loader shim needs both.
- Whether the IPL caps the read length by the EEPROM `ifsSizeInMB` field /
  `set-ifs-size`. This bounds our image size.
- The byte offset of the startup header within a slot (the IPL scans for it).

## 7. Verified directly against the ISO and extracted IFS (2026-09-30)

Sources: `swdl_boombox_6.5GT.iso` (sha256 c9dca56b..., matches the handoff),
`completeifs-premium.bin` (930aea06...), `ipl-hbas-dra526-hdisys-nand.bin`
(d39baeae...). All three ImageFS regions were extracted with the
board/clock/pin kit's extractor. Stock firmware lives in the gitignored
`firmware/` directory and is never committed.

- **DRAM size: 256 or 512 MiB, selected by EMIF4 ROWSIZE.** Stock startup at
  `0x80100A18`: `ldr [0x6D000008]; ubfx #7,#3; cmp #5` → 512 MiB, else
  256 MiB, both at 0x80000000. The IPL writes ROWSIZE 5 (`0x408016B2`) when
  CONTROL_STATUS[14:13] is **zero**, else 4 (`0x40801632`). See sec. 9 for the
  correction; an earlier version of this line had it backwards. The DTS uses
  256 MiB, which is correct on both. Detecting 512 MiB only needs a register
  *read*.
- **Display is 400x240, not 400x234.** This ISO carries only the premium
  image. Boot-IFS `display.conf` → `premium/omap3530.conf`, active line
  `hsw=28,hfp=40,hbp=60,vsw=3,vfp=13,vbp=29,ivs=1,ihs=1,ipc=1,ieo=0x4,
  pcd=0x8,ppl=400,lpp=240,...,dither=2`. The 400x234 entry in
  `display_graphics_map.json` comes from `standard/omap3530.conf`.
  **Unresolved:** pcd=8 gives 12 MHz / ~80 Hz at a 96 MHz DSS clock, versus
  the declared refresh=60 (9.03 MHz). The DSS clock source hasn't been traced.
- **QNX `/dev/i2c2` = OMAP I2C3** (`i2c-omap35xx -p0x48060000 -c400 -i61 -u2`
  in the startup script; libeeprom `i2c_port=2,i2c_address=0x50`). A second
  instance drives I2C2 (0x48072000, IRQ 57).
- **eMMC exists (not in the original findings):** `devb-mmcsd-omap3730teb ...
  ioport=0x4809c000 ... irq=83`, automounted at `/fs/mmc0`.
- **`/mnt/persistence/pre_boot.sh` hook exists:** `etc/boot.sh:115-117` runs
  it in the background if present.
- **Bluetooth chip is contested.** `wicome.cfg` sets `BT_CHIP = CSR_ROM`,
  `HCI_TRAN = H4`, while a Marvell 88W8688 driver
  (`devnp-mv8688uap-sta2x11.so`) also ships in `mnt/persistence`. The active
  configuration points to CSR for Bluetooth. Whether the Marvell part is
  populated or used is unknown.
- **GPIO170 role is contested.** The board/clock/pin handoff calls it the
  DSP's active-low reset; `display_graphics_map.json` calls it a shared
  power enable. Both agree that high means running.
- **GPIO102 edge is contested:** rising (board/clock/pin handoff) vs falling
  (`dsp_layout_map.json`).

## 8. Stock IPL behavior, verified by running it (tools/ipl-emu)

The unmodified stock IPL (d39baeae...) was run from reset in Unicorn with
simulated NAND, EEPROM and UART (see `tools/ipl-emu/README.md` for what is and
isn't modeled). With the stock `completeifs-premium.bin` in slot 0 it prints
its normal log and hands off to `0x801004A0`. Corrupted images are rejected,
and a bad slot falls back to the next one. All of this is covered by
`tools/ipl-emu/test_ipl_emu.py`.

- **NAND is x16.** The IPL reads the spare area at column 0x400, i.e. 16-bit
  word addressing (IDs 0xCA/0xBA). It reads pages through the GPMC prefetch
  FIFO using cached sequential reads (0x30/0x31/0x3F).
- **Correction: the spare-area "sequence number" is a page counter,** not one
  number shared by the whole image. Spare +4 must be 0 on the image's first
  page and increase by 1 per page (IPL 0x40203534). Bad blocks are skipped
  without counting. `boot_path_map.json` step4 `d_sequence_number_check` is
  wrong on this point.
- **Correction: slot-valid flags are one byte per slot, mirrored.** The IPL
  reads EEPROM bytes 0-7; slot i is valid iff byte[i] == {0x33,0x66,0x99,0xCC}[i]
  and byte[i+4] == byte[i]. Rejecting a slot writes 0 to both bytes.
- **The IPL writes the EEPROM on every boot:** `1 << slot` to 0xFD0
  (currentIFS) before trying a slot, plus the invalidation above on failure.
  EEPROM reads observed: 0xFD0, 0x0F, 0x0C, 0x0D, 0x00.
- **Slot positions come out as 4 / 340 / 676**, i.e. 336 blocks per slot,
  matching `nand_partition.txt`.
- **Load behavior:** the IPL reads `stored_size` bytes (not the whole slot)
  to **0x84000000**, verifies both sum-to-zero regions (one flipped bit in
  either is rejected), copies `startup_size` bytes to `image_paddr`, and
  patches the copied header: offset 0x28 (imagefs_paddr) = 0x84000000 +
  startup_size, plus slot info from offset 0x41.
- **Size limit:** stored_size - 1 must be <= 0x29FFFFF, so the image can be
  up to 42 MiB (one slot).
- **CPU state at the jump to startup_vaddr:** SVC mode, IRQ+FIQ masked, MMU
  off, D-cache off, I-cache off. r0 = 0; r1/r2 hold leftover UART3 addresses,
  not boot arguments. This is already what the Linux ARM boot protocol
  requires, apart from r1/r2 (the shim sets them).
- **Fail-open confirmed:** if every valid slot fails, the IPL prints "All
  partitions failed, retrying..." and tries all slots, including invalid ones.

## 9. RAM-size strap polarity (correction)

`boot_path_map.json` (`package_type_autodetect`, and the 0x6d000008 entry)
says the IPL uses 0x408016B2 "if CONTROL_STATUS bit 0x6000 is set". **That's
backwards.** IPL 0x40200A9C-0x40200AF8:

```
mov r3, #0x280                 ; default: ROWSIZE 5
ldr r1, [CONTROL_STATUS]; and r1, #0x6000; cmp r1, #0; beq keep
mov r3, #0x200                 ; any strap bit set: ROWSIZE 4
```

Confirmed by running it: CONTROL_STATUS 0x0000 → writes 0x408016B2 (stock
startup then registers 512 MiB); 0x2000/0x4000/0x6000 → 0x40801632 (256 MiB).
Which strap a real unit has is unknown until hardware, which is why the boot
shim reads ROWSIZE at runtime, the way stock startup does, instead of assuming.

## 10. IPL pad table (pin muxing)

Source: the stock IPL itself (body offset 0x4240, 155 entries of
{u32 address, u16 value, u16 pad}, written with `strh` by 0x40200CC4; the
table is followed by the 0xCC996633 slot-valid constant). My extraction
matches the board/clock/pin handoff's `ipl_pad_table_labeled.csv` exactly.
The pads fall in mainline's three AM3517 pin controllers: core 0x48002030
(133), core2 0x480025D8 (18), wkup 0x48002A00 (4). They're applied as-is by
`hogtied-boombox-pinmux.dtsi`, generated by `tools/pinmux/gen_pinmux.py`;
`tools/pinmux/test_pinmux.py` checks the compiled DTB programs exactly these
155 pairs.

- **IOC handshake lines are GPIO136/GPIO137.** Pads 0x2164/0x2166
  (mmc2_dat4/5) are in mode 4 (GPIO) as inputs, matching `dev-ipc -I136,137`.
  This supersedes the sec. 5 note that 136/137 are unknown remapped IRQs
  (REQ vs ACK assignment is still unknown).
- **Correction to the board/clock/pin handoff:** its README says "bit 3
  disables the pull resistor". Mainline `<dt-bindings/pinctrl/omap.h>` defines
  bit 3 as `PULL_ENA`. So 0x0118 = input + pull-up (the usual I2C/MMC
  setting, as the I2C and MMC pads here are), 0x0100 = input, no pull. Its
  value tables are right; only the decoding of bit 3 is inverted.
- Buses visible in the table: 16-bit GPMC data (d0-d15, consistent with the
  x16 NAND), 16 DSS data lines, MMC1 8-bit (the eMMC), I2C1-3, UART1-3,
  McSPI2, McBSP1-4, HECC1 (CAN). Pads in modes other than 0/4 (e.g. the etk
  pads in mode 1/2, likely MMC3/McSPI3) aren't labeled by the handoff and
  aren't named here.
- Label check against mainline board-file comments: 94 agree, 44 have no
  mainline reference, 17 "disagree" only by naming (mmc1_ vs sdmmc1_, GPIO
  alias comments). One am35xx board file's McBSP1 comments are off by one
  pad compared with the standard OMAP3 layout the handoff uses.

## 11. Vehicle CAN: arrives through the IOC, per Harley's own source

`secondary/usr/share/lua/service/vehicleCAN/vehicleCAN.lua` (plain-text Lua,
1213 lines) is the stock decoder for bike data. It reads frames from **IOC IPC
channel 4** (`chan = ipc.open(4)`; "IPC messages coming from the IOC"). It does
not read the OMAP HECC controller. This contradicts the DSP handoff's
`linux-bringup-map.md` §9, which assumes Linux reads the bike via HECC
(`io-can-shiva /dev/can1` does run in `boot.sh`, but vehicleCAN doesn't use it).

Channel-4 RX message layout (1-based Lua indices): `msg[1]` CAN ID low byte,
`msg[2]` CAN ID high byte, `msg[3]` not used by any decoder (probably the
DLC, unverified), `msg[4..11]` CAN data bytes 1-8 ("msg[4] is the first data
byte"). TX is different: `msg[1]` is an index from `CAN_INDEX_TBL` (not the
ID), `msg[2]` the length, then data.

Decoded messages (data bytes 0-based; all multi-byte fields big-endian):

| ID | Fields |
|---|---|
| 0x530 BODY_CTRL_DATA1 | [4] oil pressure (2 kPa); [6] power mode 0x20 OFF / 0x40 ACC / 0x60 IGN / 0x80 CRANK |
| 0x531 BODY_CTRL_DATA2 | [0] TPMS flags (0x80 enabled, 0x40/0x20/0x10 low batt LR/R/F, 0x08/0x04/0x02 low pressure LR/R/F, 0x01 telltale); [1]&1 trike; [2..4] tire temp F/R/LR; [5..7] tire pressure F/R/LR (254 error, 255 no value; offset/units applied by the HMI, not here) |
| 0x540 ENGINE_CTRL_DATA1 | [0..3] total distance (m, ≥0xFFFFFFFE invalid); [4] ambient (<0xFE: raw/2-40 °C); [5..7] fuel used (0.1 ml, ≥0xFFFFFE invalid) |
| 0x541 ENGINE_CTRL_DATA2 | [0..1] rpm; [2..3] speed (0.1 km/h; 0xFFFE error, 0xFFFF no value); [4..5] engine temp (units not converted in the Lua); [6] gear (value meanings not in the Lua); [7] coolant temp (units not converted) |
| 0x542 ENGINE_CTRL_DATA3 | [0]&1 overtemp; [1]&2 oil pressure telltale; [1]&0x40 engine running; [2]&2 chassis fan enabled; [2]&0x18 fan mode (0 none, 1 on, 2 off, 3 auto); [3]&8 RCCO enabled |
| 0x544 ENGINE_CTRL_DATA5 | [2]&8 RCCO active; [2]&0xC0 oil pressure mode (0 none, 1 switch, 2 sensor); [3] oil pressure (2 kPa) |
| 0x5C0 INSTRUMENT1_DATA1 | [0]&0x20 metric; [0]&0x40 low fuel; [0]&0x80 daytime lighting; [6] photocell |
| 0x5C1 INSTRUMENT1_DATA2 | [0] s, [1] min, [2] h (speedometer clock); [3..5] battery-connect time; [7]&0x80 24-hour mode |
| 0x066 LOG_SHUTDOWN | IOC shutdown reasons 1-13 (e.g. 2 = "J3 IPC Watchdog", 10 = low voltage) |

The Lua cites "MY13 HDLAN Normal Mode Message Specification" and
"Harley-Davidson_rev_4_5.xls" as its sources; we don't have those.
