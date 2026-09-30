# HogTiedOS — Hardware Reverse-Engineering Findings

Target: Harley-Davidson Boom! Box 6.5GT infotainment head unit (firmware NA 1.22.0.3).
Everything in this bundle was extracted by static analysis of the stock QNX firmware
image (`swdl_boombox_6.5GT.iso`, sha256 `c9dca56b...`). No hardware access has been
used yet; nothing here has been confirmed on a live unit.

## How to use this with Claude Code

Drop this folder's contents into `docs/findings/` in the repo, then start Claude Code
in the repo root and say something like:

> Read PROJECT_DECISIONS.md first, then every file in docs/findings/ and
> DSP_EQ_TOOL.md. This is the hardware reference and project plan for HogTiedOS,
> a replacement OS for a Harley-Davidson Boom! Box 6.5GT head unit. Confidence
> levels are marked throughout — treat "confirmed" facts as reliable and
> "unresolved"/"still open" items as real gaps, not something to guess at.
> Help me set up the Buildroot external tree and initial device tree per the
> plan in PROJECT_DECISIONS.md.

## File index

- **PROJECT_DECISIONS.md** — read this one first. Captures the *reasoning* behind
  the plan (why Linux/Buildroot, why chain-load off the stock IPL, the real-time-EQ
  architecture decision, the safety/testing plan, why the bench capture was
  deferred) — context that isn't in the raw findings files below.

- **boot_path_map.json** — IPL boot sequence, NAND geometry (2048B/64B/128KB, 4-bit BCH),
  GPMC/ECC register setup, checksum rule (sum-to-zero, no crypto signature), SoC
  identity (undocumented DRA523/DRA526 "Jacinto 3"), DDR controller status (register
  writes captured, semantics UNRESOLVED — treat as a real gap, not a TODO to fill in casually)

- **dsp_layout_map.json** — Audio DSP (Sigma200-family) SPI wire protocol, safe-load
  mechanism, parameter layout table with real addresses pulled from firmware, EQ file
  format and checksum rule, full command table. Real-time EQ updates are architecturally
  supported (separate biquad-only command from full-profile-load command).

- **ioc_link_map.json** — Front-panel/IOC microcontroller link: SPI3 + IPC channel map,
  a working power-reset command (2 raw bytes to channel 2), button-state bit layout
  quoted from an internal Harman spec referenced in firmware comments.

- **display_graphics_map.json** — Display/GPU: confirmed SGX530 GPU, full MMIO register
  map (matches mainline OMAP3530 — unlike the DDR controller), DISPC soft-reset sequence,
  active panel timing (400x234, NOT 400x240 — see file for the caveat on which variant
  is truly active), GPU is optional (stock ships a working no-GPU config).

- **bt_wifi_gps_map.json** — Marvell 88W8688 (SDIO, confirmed by driver strings) for
  BT/WiFi, u-blox G5/G6 GPS with a complete ready-to-implement init sequence (exact
  UART bytes), SDARS/SiriusXM UART + GPIO-gated presence detection.

- **DSP_EQ_TOOL.md** / **eqtool.py** — Working, tested Python tool that validates,
  repairs checksums on, and dumps EQ profile files from this firmware. Passes all
  132 factory profiles. Never touches hardware.

## Cross-file notes worth keeping in mind

- **GPIO170** appears in both the DSP and display findings — it's a shared,
  ignition-gated power-enable output (not a sense pin; confirmed configured as
  output-only in the IPL pad table), asserted by software AFTER an ignition-on
  message arrives over CAN, not something to poll directly.
- **No file in this bundle should be treated as "verified on hardware."** Confidence
  levels are stated per-finding. Several items are flagged as needing either bench
  access or vendor documentation that isn't available.
- The recommended OS strategy (see boot_path_map.json's `linux_port_implications`)
  is to package the OS image in the same QNX-header format so the **stock IPL loads
  it unmodified** — no custom bootloader DDR init needed, sidestepping the one
  genuinely undocumented part of this SoC.
