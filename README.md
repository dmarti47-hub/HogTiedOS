# HogTiedOS

A replacement embedded Linux OS for the Harley-Davidson Boom! Box 6.5GT
infotainment head unit (TI DRA523/DRA526 "Jacinto 3", OMAP3-family). It is
reverse-engineered from the stock QNX firmware (NA 1.22.0.3).

**Status:** build-system bring-up. Nothing has run on hardware.

- `docs/findings/`: hardware findings and project decisions. Start with
  `PROJECT_DECISIONS.md`, then `CROSS_CHECKS.md` for corrections.
- `buildroot-external/`: Buildroot external tree (defconfig, device tree,
  kernel patch). Build instructions are in its README.

Core strategy: package the OS so the **stock IPL boots it unmodified** (it
already does DRAM bring-up on undocumented silicon), and never write the IPL,
the DRAM controller, or all three IFS slots.
