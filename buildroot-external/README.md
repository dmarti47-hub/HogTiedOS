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

**After editing the DTS, kernel patch or fragment**, run
`make -C buildroot-2025.02.18 O=$PWD/output linux-rebuild all`. Buildroot
doesn't notice changes to external-tree kernel inputs, and a plain `make`
silently packs the old DTB.

Output lands in `output/images/`:
- `hogtied.ifs`: the bootable image for one IFS slot. It's built and validated
  by `board/boombox/post-image.sh` using `tools/ifs-pack/`.
- `zImage` (initramfs built in) and `hogtied-boombox.dtb`: its inputs.

**Nothing in this tree writes to hardware.** Getting `hogtied.ifs` onto the
unit is a separate, manual step through stock `update_nand_teb` (see
`docs/findings/PROJECT_DECISIONS.md`). Before that, check the image against
the real stock IPL in the emulator (`tools/ifs-pack/test_mkifs.py`).

## Layout

```
configs/hogtied_boombox_defconfig   Buildroot config
board/boombox/
  dts/hogtied-boombox.dts           board DT on mainline am3517.dtsi
  linux.fragment                    kernel config over omap2plus_defconfig
  patches/linux/0001-*.patch        keep the kernel off the DRAM controller
package/hogtied-ui                  main screen (software/hogtied-ui + libhbas)
package/hogtied-lvgl                LVGL 9.2.2 source for hogtied-ui
```

## Safety properties built into this tree

- **No DRAM-controller writes from Linux.** The kernel patch skips
  `omap_sdrc_init()` and OMAP3 PM/idle for `hogtied,boombox-6.5gt`. The
  fragment disables cpufreq, cpuidle and SmartReflex. See
  `docs/findings/CROSS_CHECKS.md` sec. 2.
- **NAND is disabled in the DT, and every partition is `read-only`**,
  including all four IPL copies. Flashing only ever happens through stock
  `update_nand_teb`, per `docs/findings/PROJECT_DECISIONS.md`.
- **The boot-state EEPROM (I2C3, 0x50) is `read-only` in the DT.** It holds
  the IFS-slot valid flags the IPL uses to pick a slot.
- **The boot shim writes nothing but UART3** (tested). Its only other
  hardware access is one read of EMIF4 SDRAM_CONFIG to detect 512 MiB units,
  the same read stock startup does.

- SoC base (`am3517.dtsi`) is tentative. It gets confirmed from the kernel's
  IDCODE print on the first UART boot.
- Pin settings are applied as one block per pin controller (exactly the
  IPL's table), not yet split per device.
- The IOC link (bike data, handlebar buttons, power heartbeat) isn't
  implemented, so the screen shows `--` on the unit.
- Display pixel clock is unresolved (pcd=8 vs refresh=60).
- The DSP and IOC SPI devices are placeholders with no drivers.
