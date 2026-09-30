# HogTiedOS Buildroot external tree

`BR2_EXTERNAL` tree (name `HOGTIED`) for the Boom! Box 6.5GT. Targets
**Buildroot 2025.02.x LTS** and **Linux 6.18.54 LTS**.

## Build

```sh
# once: get Buildroot next to this repo (or anywhere)
curl -LO https://buildroot.org/downloads/buildroot-2025.02.18.tar.xz
tar xf buildroot-2025.02.18.tar.xz

make -C buildroot-2025.02.18 BR2_EXTERNAL=$PWD/buildroot-external O=$PWD/output hogtied_boombox_defconfig
make -C buildroot-2025.02.18 O=$PWD/output
```

Output lands in `output/images/`: `zImage` (with the initramfs built in) and
`hogtied-boombox.dtb`.

**This output is not bootable yet, and nothing in this tree writes to
hardware.** It still needs the QNX-format IFS packer (see below).

## Layout

```
configs/hogtied_boombox_defconfig   Buildroot config
board/boombox/
  dts/hogtied-boombox.dts           board DT on mainline am3517.dtsi
  linux.fragment                    kernel config over omap2plus_defconfig
  patches/linux/0001-*.patch        keep the kernel off the DRAM controller
package/                            custom packages (none yet)
```

## Safety properties built into this tree

- **No DRAM-controller writes from Linux.** The kernel patch skips
  `omap_sdrc_init()` and OMAP3 PM/idle for `hogtied,boombox-6.5gt`. The
  fragment disables cpufreq, cpuidle and SmartReflex. See
  `docs/findings/CROSS_CHECKS.md` sec. 2.
- **NAND is disabled in the DT, and every partition is `read-only`**,
  including all four IPL copies. Flashing only ever happens through stock
  `update_nand_teb`, per `docs/findings/PROJECT_DECISIONS.md`.
- **The boot-state EEPROM (I2C 0x50) is disabled and `read-only`.** It holds
  the IFS-slot valid flags the IPL uses to pick a slot.

## Known gaps (see TODO markers in the DTS)

- DRAM size is unknown. The memory node has size 0 on purpose.
- SoC base (`am3517.dtsi`) is tentative. It gets confirmed from the kernel's
  IDCODE print on the first UART boot.
- There's no pinctrl, because the IPL pad table hasn't been extracted.
- The DSP and IOC SPI devices are placeholders with no drivers.
- Panel timing encoding (real counts vs. DISPC register values) is unconfirmed.

## Not written yet: IFS packer

A post-image step has to wrap `zImage` + DTB in a QNX startup-header image
the stock IPL accepts (signature `0x00FF7EEB`, two sum-to-zero regions) with
a small loader shim at `startup_vaddr`. It also needs a validator that runs
before any image goes near NAND. Several of its inputs are still open; see
`docs/findings/CROSS_CHECKS.md` sec. 6.
