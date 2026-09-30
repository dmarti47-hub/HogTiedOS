# HogTiedOS — Project Decisions & Reasoning

This captures the *why* behind the plan, not just the hardware facts (those are in
the other files under `docs/findings/`). This is context from the research
conversation that led to this repo — read it before making changes that contradict
it, since these were deliberate calls, not defaults.

## The core strategy: don't fight the boot chain, ride it

The single most important decision: **the replacement OS image will be packaged in
the same QNX-compatible format the stock IPL already expects** (correct signature
bytes, size fields, and a checksum word chosen so the image sums to zero — see
`boot_path_map.json`), so the **stock IPL loads it unmodified**.

Why this matters: the IPL already does the hard, undocumented parts for us —
NAND geometry handling, ECC, ECC-marked page sequencing, and critically, **DDR
controller bring-up**. The SoC (DRA523/DRA526, "Jacinto 3") is genuinely
undocumented — TI never published a public datasheet for it, and its DDR/SDRC
register layout doesn't match mainline OMAP3530 (confirmed by direct comparison
against public register references, not assumed). Writing our own DDR init would
mean guessing at an undocumented memory controller, which is a real brick risk.

**Rule: never write custom DDR/SDRC init code.** If the OS needs to run before
the IPL would normally hand off, that's a red flag — restructure to let the IPL's
own DRAM bring-up complete first.

## Why Linux (Buildroot), not something custom or RTOS-based

Reasoning, not just a conclusion:
- The peripheral set (GPMC NAND, McSPI, McBSP, SDIO, UART) is standard OMAP3-family
  hardware with mature mainline Linux drivers. Writing our own equivalents from
  scratch would be reimplementing solved problems for no benefit.
- The one subsystem that *didn't* match mainline OMAP3530 documentation was DDR —
  and the plan already avoids touching that (see above). Every other subsystem we
  checked (display MMIO map, NAND geometry, GPMC/ECC) matched published addresses
  directly.
- Android was considered and rejected: SGX530 is an old, poorly-maintained GPU on
  modern kernels, and a 400x234 gauge-cluster-style display doesn't need Android's
  weight. Stock firmware itself proves the UI works fine with **no GPU at all**
  (`flash_nosgx.conf` is a real, complete, working config — not a stub) — so we're
  deliberately skipping SGX/PowerVR driver work entirely and using a lightweight
  framebuffer-based UI toolkit (LVGL is the current leaning) instead.
- Buildroot over Yocto: this is a single fixed hardware target, not a product
  line. Yocto's package-management sophistication isn't needed here, and
  Buildroot's simpler, faster iteration loop matches an exploratory
  reverse-engineering-driven project better.

## Real-time EQ is architecturally supported — build for it

Confirmed from the DSP command table: `0x905` (full profile load) and `0x907`
(biquad-only update) are *separate* dispatch entries, and the write path uses the
DSP's safe-load registers (write data, write target address, write trigger) —
a mechanism that exists specifically to update filter coefficients glitch-free
*while audio is playing*. A boot-time-only design wouldn't need safe-load at all.

**Design implication:** build the audio driver so EQ changes go through the
biquad-only path (`0x907`-equivalent) at runtime, not by re-sending a whole
profile. This was a deliberate architectural choice based on what the protocol
supports, not something confirmed by hearing it happen on real hardware yet.

## Why we stopped pursuing a bench capture (for now)

We considered a logic-analyzer capture of the DSP SPI bus to confirm the
parameter-address table before writing driver code. Decision: **not worth doing
as a separate research step.**

Reasoning: the addresses in `dsp_layout_map.json`'s layout table aren't a guess —
they're the actual static data the stock firmware itself reads from at runtime.
A bench capture would confirm behavior, not discover addresses we don't have.
Since the head unit would need to be powered up for real testing anyway once
there's working code to test, a separate disconnected bench-capture phase first
was redundant effort. **Verification is folded into normal testing of the first
working build, not treated as a prerequisite research phase.**

This may need revisiting if a first implementation doesn't produce expected
audio behavior — at that point, targeted hardware debugging (UART console,
possibly a scope on specific lines) becomes a debugging tool for a known
failure, not exploratory research.

## Safety plan for testing on real hardware

Decided plan, to avoid bricking the unit during iteration:

1. **Never write to the IPL.** All custom-OS writes go to IFS slots only. The IPL
   is what provides automatic fallback (see below) — if it's ever corrupted,
   there's no recovery path beneath it.
2. **Always keep at least one IFS slot stock.** Three slots exist
   (`boot_path_map.json`: blocks 4-339, 340-675, 676-1011). Never overwrite all
   three with test builds. One slot is always a known-good fallback.
3. **Use `set-partition` to mark a slot invalid before writing it, valid only
   after confirming the write succeeded** — mirroring exactly what the stock
   SWDL updater does. A failed/interrupted write just gets skipped by the IPL's
   own boot-time fallback logic (tries slots in rotation, skips invalid ones)
   automatically.
4. **Validate the image's checksum in software before writing it to NAND** —
   the sum-to-zero rule (`boot_path_map.json`) is simple to check on a
   dev machine. Same principle as `eqtool.py`'s EQ-file validation; a similar
   validator should exist for full IFS images before this project needs to
   flash real hardware.
5. **Use `update_nand_teb` (the stock flashing tool) rather than raw NAND
   writes** — it already handles bad-block marking and page/ECC sequencing
   correctly; a naive raw write would skip all of that.
6. **UART3 debug console (115200 8N1) is the first thing to wire up**, before
   any other physical access work — confirms boot progress/failure without
   needing to guess, and is low-risk (signal-level pads, not power/audio path).

Net effect of this plan: a bad test build should fall back to a known-good stock
slot automatically, not brick the unit. "Bricked" should only be possible via IPL
corruption, which nothing in this workflow ever touches.

## Physical access reality check

- Head unit likely needs to come off the bike for bench work eventually, but
  **this is not required to start writing code** — driver work against the
  documented protocols can proceed without hardware in hand.
- UART wiring is low-risk (signal pads, not power-carrying) — non-destructive
  probing (spring test clips) is preferred over soldering where practical.
- The USB-Ethernet diagnostic path (DID 0xFD07, see `bt_wifi_gps_map.json`) was
  evaluated and set aside: it requires a specific ASIX-chipset dongle (D-Link
  DUB-E100 confirmed by firmware string match; a Realtek-chipset dongle like the
  TP-Link UE300 will NOT work) *and* a CAN diagnostic write capability that may
  not be available. UART is the more direct path given current tooling on hand.
- No public teardown/UART-pad documentation exists for this exact board (checked
  and confirmed absent) — pad locations will need to be found via board
  inspection or continuity testing when that phase starts.

## Status snapshot at time of handoff

**Solid and ready to build against:** boot chain (no signature check, checksum
rule known), NAND/GPMC/ECC config, front/IO-controller SPI+IPC protocol and
channel map, GPS init sequence (exact bytes), Wi-Fi/BT chip ID and SDIO config,
display MMIO map and reset sequence, EQ file format (tool exists and passes all
132 factory files).

**Real, acknowledged gaps — not yet resolved, don't guess at these:**
DDR/SDRC controller semantics (undocumented silicon; mitigated by not touching
it — see above), DISPC timing-register programming (searched exhaustively,
genuinely not found in the traced binary — may be a different code path
entirely), exact DSP boot method (how the Sigma200 gets its program — no bulk
download path exists from the host, so it's self-booting from something on-board
or fixed-program; not confirmed which), brightness devctl exact byte format,
GPS config blob (`g5.cfg`) is compressed/encrypted and not decoded.

None of the gaps are believed to block a first bootable build; they matter for
specific features (custom DDR reinit, live display-timing changes, exact
brightness protocol) that aren't required for initial bring-up.
