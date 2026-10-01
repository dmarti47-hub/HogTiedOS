# Bench bring-up plan

What we need to learn on the physical Boom! Box 6.5GT, and the order to
learn it in so nothing is risked before it has to be. Nothing in here is a
step we run remotely: every item that writes to the unit waits for David's
explicit go-ahead at the bench.

## Ground rules (unchanged from the project constraints)

- **No write to the unit without explicit confirmation, every time.**
- **Always keep one known-good stock IFS slot.** Never let an experiment
  reduce us below one bootable stock image.
- **Never touch the IPL or DDR/SDRC init.** We rely on the stock IPL having
  already brought up RAM; we never write that layer.
- **Flash only through the stock `update_nand_teb` path.** We don't invent a
  new writer.
- **Reversibility before writing.** We don't make a change we don't already
  know how to undo.
- **Escalate slowly:** observe → read → one small reversible write → full
  install. Each step has a stop condition.
- No debug port / JTAG / factory UART is assumed available. Everything here
  works without one. The Phase 0a teardown looks for an exposed serial/JTAG
  footprint — if one turns up it changes the bring-up for the better, but
  the plan doesn't depend on it.

## What we need to learn, ranked

0. **Board survey.** Chip part numbers and board layout, read directly.
   This flips the project's "no board markings" assumption: several things
   we've refused to guess (DDR geometry, eMMC/NAND size, touch controller)
   become datasheet lookups, and a teardown may expose a serial/JTAG
   footprint that changes the whole bring-up. Non-destructive, so it goes
   first.
1. **Power & baseline boot.** How to run the unit on a bench, and that it
   boots stock cleanly there. Everything else is meaningless without a
   known-good reference.
2. **Recovery story.** If a slot is left bad, how does the unit recover to
   the stock slot? Confirm this *exists and works* before we ever write.
3. **What the update path enforces, and where.** Checksum/format vs. a true
   cryptographic signature, and whether it's checked at update time or at
   boot. This single answer decides whether a normal install is even open to
   us (see the earlier discussion — if it's a real signature, we do **not**
   try to defeat it; we fall back to legitimate routes).
4. **Our payload boots.** Package HogTiedOS to whatever the path accepts and
   bring it up from the non-stock slot, stock slot as fallback.
5. **Subsystem bring-up** (only once our own code runs): eMMC layout & free
   space, display/touch, audio (McBSP4 → DSP), CSR Bluetooth init, CAN/IOC,
   GPS UART.

## Equipment / setup to prepare beforehand

- ESD strap and mat.
- A camera that can do macro / close focus, and raking light (a phone
  torch at a low angle) to read part numbers through any conformal coating.
- Small driver set; a tray and labelled photos for screws and the
  disassembly order so reassembly is clean.
- Bench 12 V supply with **current readout** and a current limit set a bit
  above the unit's normal draw (so a fault trips the supply, not the board).
- The vehicle harness connector pinout for this head unit: switched/ACC,
  constant 12 V, ground, and anything the unit needs to not fault on the
  bench (it may expect CAN presence or an illumination/ground sense). Learn
  this from the connector before applying power.
- A USB stick formatted the way the stock updater expects.
- An untouched copy of the stock update package (from `firmware/`, never
  committed) as the known-good reference, plus a second working copy for the
  characterization in Phase 2.
- A way to read anything we write back out (the same USB stick, read on the
  PC).

## Phase 0a — Teardown and board survey (no power)

Question: what is actually on the board, and is a console exposed? This is
the one teardown; capture everything so we never have to open it again to
answer a question we could have photographed.

Handling: ESD strap on. The LCD + touch digitizer usually lifts off as an
assembly on fragile ribbon cables — photograph each connector's position
and orientation before unseating anything. Automotive boards often have
conformal coating; read part numbers with raking light, never scrape.
External photography only — nothing destructive, no decapping. Don't power
the board while it's apart (the SoC and audio amp may need the chassis for
their thermal path).

Capture, in sharp, legible photos:

1. **Every major chip's full top marking** (line by line — the whole
   number, not just the family), for at least:
   - SoC (confirm the DRA526 / AM35xx-family part and its revision)
   - **DRAM** — the part number gives us DDR type, width and size, the
     geometry we've refused to guess. Photograph every DRAM package.
   - eMMC and the NAND that holds the IFS slots — parts → sizes
   - CSR Bluetooth (against our PS-key / build-ID assumptions)
   - u-blox GPS module
   - SigmaDSP (against our register map) and the audio codec/amp on McBSP4
   - PMIC (matters for safe bench power sequencing)
   - the EEPROM (holds `touchCal` and the BD_ADDR)
   - **the touch controller** — reading this answers the touch unknown
     outright
2. **Unpopulated footprints and test points.** Look hard near the SoC for a
   3–4 pin header (UART: TX/RX/GND, maybe VCC) and a TI JTAG/cJTAG footprint
   (14/20 pin), and for labelled test pads. Photograph silkscreen labels.
   A usable serial console here would change the whole bring-up — flag it.
3. **The main harness connector**, and trace which pins reach the PMIC /
   power input. This is how we get the Phase 0b power pinout safely.
4. **Overall board, both sides**, and the disassembly order as you go.

Deliverable: a labelled set of photos and a parts list (chip → part number
→ what it tells us). This feeds straight into `docs/findings/`.

## Phase 0b — Power and non-destructive observation (no writes)

Question: does it run and boot stock on the bench, and what interfaces are
live?

1. Wire power per the learned pinout. Bring it up on the current-limited
   supply. Record idle and boot current.
2. Let it boot stock. Record: time to UI, boot stages visible on screen, any
   fault/limp behavior from missing CAN, and whether it settles.
3. Inventory exposed interfaces without opening anything up: USB port
   behavior (does it enumerate a host port for media/updates?), SD slot if
   present, screen, buttons.
4. Repeat a cold boot 2–3× to learn what "normal" looks like.

Stop condition: if it won't boot stock cleanly on the bench, we fix the
bench setup (power/CAN presence) before anything else. Do not proceed.

## Phase 1 — Known-good update, end to end (stock image only)

Question: can we drive the stock update flow ourselves and watch it succeed?

1. Put the **unmodified** stock update package on the USB stick and run the
   stock update exactly as a dealer/owner would.
2. Watch the whole flow: how it's triggered, progress/version shown, how long
   it takes, how it reports success, how it behaves across the reboot.
3. Confirm the unit comes back up stock and healthy.

This is our reference run. We change nothing about the unit yet. Record
every observable so a later failed/odd run is diagnosable by comparison.

## Phase 2 — Characterize what the update path checks (reversible)

Question: at what layer is image integrity enforced — a checksum/format, or
a real signature — and at update time or at boot? We're *reading the
behavior of the gate*, not defeating it.

Work only on a **copy** of the stock package, keep a stock slot intact, and
make each change the smallest reversible one, observing accept vs. reject:

1. **Payload-region change:** flip a byte in a data/payload area and present
   it. If it's **accepted and runs**, the path tolerates content changes in
   that region (integrity isn't covering it) — useful, and it means our
   content can live there.
2. **Header/checksum change:** change a header or checksum field and present
   it. The **error it gives** tells us a checksum exists and roughly where /
   what it covers.
3. From the pattern of accept/reject, classify the gate:
   - **Checksum / format only** → a normal install is a matter of matching
     the container format and recomputing the checksum. Legitimate
     reverse-engineering of a file layout; this is the good outcome.
   - **True cryptographic signature** (update-time or boot-time) → the normal
     path is closed to unsigned images and **we stop here on this track.** We
     do not attempt to forge or bypass it. We fall back to: our payload
     riding inside a slot the stock chain already accepts (current model), a
     documented platform peripheral-boot/engineering mode if one exists, or
     an authorized reflash channel.

Hard rule for this phase: never present a modified image to the *last*
stock slot. Always keep one untouched stock image to recover to.

## Phase 3 — Recovery, proven before we rely on it

Question: if a non-stock slot is bad, does the unit fall back to the stock
slot on its own, or is there a recovery reflash?

1. From Phases 1–2 we should understand slot selection. Confirm the fallback
   path in the least-risky way available — ideally by observation and by
   re-running the stock update as recovery, **not** by deliberately bricking
   a slot.
2. Establish the "unbrickable floor": confirm that nothing in the accepted
   update path can reach the IPL/DDR layer we never touch. If we can't
   confirm that, we don't do the first payload write.

Only with a proven way back do we move to Phase 4.

## Phase 4 — Boot our payload

Question: does HogTiedOS boot from the non-stock slot via the stock chain?

1. Package HogTiedOS to whatever Phase 2 showed the path accepts.
2. Write it to the **non-stock** slot (explicit confirmation), stock slot
   untouched as fallback.
3. Boot and observe. Without a debug port, our observation channels are:
   - **The screen** — bring the UI (or an early text diagnostic) up as soon
     as possible and print progress.
   - **A USB gadget console** — if we can bring up USB device-mode from our
     payload (serial or ethernet gadget), that's a real console without any
     debug port. Worth trying early.
   - **Log-to-storage** — write a boot log to the eMMC or the USB stick and
     read it back on the PC if the screen stays dark.
4. If it doesn't come up, fall back to the stock slot and read whatever log
   we captured.

## Phase 5 — Subsystem bring-up (once our code runs)

Each of these is its own observe-first exercise, and several are already
documented as hardware-only unknowns:

- **eMMC:** partition layout and actual free space (settings/maps/BT store
  all depend on this).
- **DDR:** confirm the stock IPL already configured RAM and we never write
  it — a read-only sanity check, nothing more.
- **Display/touch:** panel timing; identify the touch controller and recover
  its calibration (`touchCal` in the EEPROM) before `--touch` is useful.
- **Audio:** the McBSP4 link to the DSP; replay the CSR Bluetooth init;
  DSP profile loader.
- **CAN/IOC and GPS UART:** confirm against what we modelled from the ISO.

## Decision summary

The gating question is Phase 2's classification. Everything after Phase 3
waits on a **proven recovery path**. If Phase 2 says "true signature," the
normal-install track ends there and we stay on the ride-inside-an-accepted-
slot model — we don't go looking for a way around the signature.
