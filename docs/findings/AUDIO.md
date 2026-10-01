# Audio: stock behaviour, what HogTiedOS implements, and the bench capture

Sources: stock `PRE_CAL/cfg/audioMgr.cfg` and `audioCtrlSvc.cfg` (JSON with
Harley's own descriptions), `usr/share/lua/service/eqService/eqService.lua`,
`bin/audioCtrlSvc` (disassembled with `tools/re/qnxdis.py`), and
`dsp_layout_map.json`. Section 7 is static analysis of `audioCtrlSvc`
(sha256 a199b880...). Nothing here has been observed on hardware.

## 1. User settings in the stock radio

| Setting | Range | Step → dB (stock table) | Source |
|---|---|---|---|
| Volume | 18 steps, 0..17, default 5 | `[-100, -41, -34, -29, -23, -18, -13, -9, -5, -1, 2, 5, 8, 10, 12, 13, 15, 16]` | audioCtrlSvc.cfg `volume_ctrl_step_curve` |
| Bass | 17 steps | step N → N dB (0..16) | audioMgr.cfg `bass_response_curve` |
| Treble | 17 steps | step N → N dB (0..16) | audioMgr.cfg `treble_response_curve` |
| Fade | 17 steps | front `[-100,-34,-24,-16,-10,-6,-4,-2,0,...]`, rear mirrored | audioMgr.cfg `fade_response_curve` |
| Output routing | speakers, driver headset, passenger headset (+ a 4th headset) | per-source routing table | audioMgr.cfg `audio_routing` |

Volume is stored per output × source (media, phone, nav, CB/intercom TX, CB
RX, ringtone, beep), all defaulting to step 5. Other calibrated items:
per-source gain matching (`source_gain_matching`), mute and mode-change ramps
(80 ms), VOX thresholds (18 steps, speed-dependent offset), sidetone levels,
and configurable beeps.

(Which bass/treble step is "flat" no longer matters: HogTiedOS replaces
bass/treble with its own EQ, sec. 4.)

## 2. Automatic EQ (eqService.lua)

The factory EQ profile is picked automatically, never by the rider:

- speakers: `/mnt/persistence/eq/<bikecfg>_<ON|OFF>.bin` (legacy name
  `<bikecfg>_<ON|OFF>_<model>_<speakers>.bin`)
- headsets: `HS_<ON|OFF>.bin`

`ON`/`OFF` is **engine running**, so the EQ changes when the engine starts or
stops. `<bikecfg>` is `HD_Configuration_Options` value[1] (`%02d`, 00..255),
which comes from IOC diagnostic identifiers that HogTiedOS doesn't decode yet.
183 profile files ship (`PRE_CAL/eq/`), and `eqtool.py` validates them. Each
config also has `<bikecfg>.conf` listing which of 4 external amplifiers exist.

## 3. Stock DSP operations (devctl handlers inside audioCtrlSvc)

audioCtrlSvc both issues these devctls and implements them: each handler
turns its request into SPI writes on `/dev/spi0` (McSPI2). The error message
at each call site names the operation:

| Operation | devctl | Layout table index → DSP address | What stock uses it for |
|---|---|---|---|
| `DSP_REGISTER_IO_HANDLE` | 0x901 | - | setup |
| `DSP_LOAD_EQ` | 0x905 | tables 8, 11, 13, 20, 22 | factory EQ profile |
| `DSP_SET_OUTPUT_GAIN` | 0x906 | 12 → **1109 + ch**, 10 channels | fade (ch 0-1 front, 2-3 rear), speed-based volume (AVC) |
| `DSP_SET_TONE` | 0x907 | 4 → **222 + 5·slot**, 7 slots, safe-load | the 7 "fixed tone" filters, incl. bass/treble (sec. 7.3) |
| `DSP_SET_INPUT_GAIN` | 0x90C | 2 → 123 + n | source gain matching |
| `DSP_SET_MAIN_AUDIO_MUTE` | 0x913 | 14 → **1037 + ch**, 10 channels | mute (gain words) |
| (mute ramp rate) | 0x914 | 14 | one of 24 discrete ramp times |
| `DSP_SET_STATIC_BIQUAD_CHAIN` | 0x925 | 19 | fixed filters |
| `DSP_SET_CLKLESS_XBAR_MIXER` | 0x92A | 21 → **295 + 16·input + output** | main_mixer1: **volume** per source and output (sec. 7.1) |
| (mixer input select) | 0x92B | 21 | 10-bit mux word |

The stock code also has an **AVC level** adjustment (speed-sensitive volume)
and a built-in fallback `Harley_FlatEQ_withToneLoudness.EQF`.

## 4. What HogTiedOS implements now

- `software/libhbas/audio.c`: settings model with the stock ranges and tables,
  per-output volume, fade only on 4-speaker bikes and only to speakers, and
  the stock EQ file naming (tested).
- `hogtied-ui` Audio page: Volume, Fade, Output, Headset. **Bass/treble are
  dropped on purpose**: the user EQ below replaces them.
  - **Output = Stock speakers:** Harley's factory EQ for the bike model
    (engine on/off), stock volume curve (up to +16 dB).
  - **Output = Custom system** (aftermarket speakers/amp): the factory speaker
    EQ is bypassed (built-in flat), and the signal never clips. The volume
    curve is the stock one shifted down 16 dB (every step still changes the
    level; the top step is exactly 0 dB), and the level drops by the
    largest user-EQ boost. The amp's gain sets how loud full volume is.
    Fade stays available. Harley's own config agrees: "0dB is maximum
    recommended input into the DSP ... Values over 0 dB can cause digital
    clipping" (audioCtrlSvc.cfg `volume_ctrl_step_curve`).
  - **Headset (Off / Driver / Passenger):** the Harley comm system's wired
    helmet jacks (`MIX1_HDST_D/P`, alongside intercom/CB, mics, sidetone and
    VOX). Media plays there when one is selected, with its own volume, the
    `HS_<ON|OFF>.bin` profile, and the stock curve. Bluetooth headsets pair
    with the phone and don't involve the head unit.
- **Bike detection:** the IOC sends DID 0xF1E8 HD_Configuration_Options on
  channel 2 at startup (`[3, 0xE8, 0xF1, value...]`, onOff.lua
  `diag_identifier`). hbas-iocd publishes byte 0 to `/run/hbas/bike`; the UI
  takes the model name, stock speaker count, trike flag and factory EQ file
  from it (`libhbas/bike.c`, from onOff.lua's `Bike_Config_Enum`).
- `hogtied-ui` EQ page + `software/libhbas/eq.c`: a **7-band** graphic EQ
  (63, 160, 400 Hz, 1, 2.5, 6.3, 16 kHz; ±10 dB; Q 1.05 for the ~1.33-octave
  spacing) with presets Flat (default), Harley (sec. 7.5), Bass, Vocal and
  Highway, made of RBJ peaking biquads at 48 kHz (confirmed,
  sec. 7.2), encoded in the verified DSP format (sec. 6) and packed into one
  35-word safe-load to the tone slots. See sec. 7.3 for what those slots
  hold in stock.
- `software/libhbas/dsp.c`: the exact parameter writes for volume, mute and
  fade (sec. 7.1), with stock's dB-to-gain conversion.
- **Backend: logging only.** Every change logs the DSP writes it would make
  (addresses and words), but nothing is sent: there's no DSP SPI writer yet,
  and the map hasn't been confirmed on hardware.

## 5. Bench capture checklist (shrunk by sec. 7)

Goal: confirm the static map, and capture what code alone can't show.

**Setup:** stock firmware, unit on the bench, logic analyzer on the DSP SPI bus
(McSPI2: CLK, SIMO, SOMI, CS0; 6 MHz, mode 0, 8-bit, CS held for the whole
transfer) plus GPIO170 (power/reset) and GPIO102 (DSP completion input).
Pads are known (IPL pad table: 0x21D6/0x21D8/0x21DA/0x21DC), physical test
points aren't. Record with timestamps, and note each HMI action as it's made.

| # | Do this on the stock HMI | Expect (from sec. 7) | Still gives us |
|---|---|---|---|
| 1 | Power up, wait for audio | writes to all tables; GPIO170 / GPIO102 timing | **DSP boot/reset sequence and the full initial parameter state** (the main remaining unknown: our backend must reproduce or keep it) |
| 2 | Volume up/down a few steps | 0x92A: words at 295 and 312 = gain of the volume curve's dB | confirmation only |
| 3 | Fade full rear → full front | 0x906: 1109-1112 | confirmation, plus whether a per-channel trim (FrontSpeakerOutputLevel / RearSpeakerOutputLevel) is folded in |
| 4 | Mute / unmute | 0x913: 1037+ch; 0x914 ramp | which of the 10 mute channels are which (HogTiedOS mutes through the mixer instead, so this is optional) |
| 5 | Start / stop engine | 0x905 profile load | confirms automatic EQ switching |

Dropped from the list: bass/treble (coefficients, sample rate and slot use
are all in the profiles, sec. 7.2-7.3), headset routing (mixer columns are
known), source changes and AVC (not used by HogTiedOS yet; their code paths
are identified).

## 6. Biquad coefficient format, verified against all factory profiles

Decoding every tag-0x36 biquad in the 183 shipped profiles (`eqtool.py`,
Q5.23, file order A1 A2 B1 B0 B2) gives 1372 non-trivial filters:

| Interpretation of stored A1/A2 | Stable | Median DC gain |
|---|---|---|
| **negated** (`a1 = -A1`, `a2 = -A2`, SigmaDSP convention) | **1372 / 1372** | **0.0 dB** |
| as stored | 42 / 1372 | -62 dB |

So the DSP stores the feedback coefficients negated. The factory filters
also satisfy B1 = -A1 exactly, the RBJ peaking-EQ relation (b1 = a1), which
independently confirms eqtool's field order. Wire order to the DSP is B2 B1 B0
A2 A1 (`dsp_layout_map.json`, biquad bank = table index 4, first biquad at
DSP address 222, stride 5, at most 7 per set; the 0x907 handler confirms
slot i → 222 + 5i, sec. 7.3).

## 7. Static analysis of audioCtrlSvc (2026-10-01)

### 7.1 Gains: volume, fade, mute

- **Word format.** Every gain is linear. Host value 2^24 = 1.0; the write
  routine (0x12b7c0) halves it, so the DSP gets **Q5.23 (0x00800000 = 0 dB)**
  as `00 | addr_hi addr_lo | 4 bytes`, top nibble zeroed (28-bit words).
- **dB → gain** (`gain_services_calc_gain`, 0x128bb0) is a table lookup at
  0x142154: 10^(dB/20) in exact 1 dB steps, 0 to -99 dB and 0 to +32 dB;
  -100 dB = 0 (off). The DSP handlers clamp to +24 dB. `floor(10^(dB/20)·2^24)`
  reproduces the table to within 1 LSB.
- **Volume = main_mixer1 crosspoints** (0x92A handler 0x126c44): word at
  `295 + 16·input + output`. Inputs (rows, from the name table at 0x13de70
  and the channel masks at 0x13df90): 0 MEDIA L, 1 MEDIA R, 2 PHONE,
  3 NAV/VR prompts, 4 driver VOX, 5 passenger VOX, 6 CB RX, 7 BEEP,
  8 RINGTONE, 9 TA. Outputs (columns, table at 0x13df60, expanded L/R by
  `setMixer`): 0-1 SPKRS L/R, 2-3 HDSTS_D, 4-5 HDSTS_P, 6-7 HDSTS_S. Stereo
  inputs go straight across (L→L, R→R). harleyManager (audioMgr) sends the
  per-output, per-source volume as these crosspoint gains
  (`updateMainMixer1` → `setMixer` batches; `default_volumes` is
  [output][input]). So media to the speakers is **295 (L) and 312 (R)**.
- **Fade = output gain** (0x906 handler 0x12a15c): word at `1109 + ch`.
  `setFade(front, rear)` sets ch 0-1 to the front value and 2-3 to the rear
  value (each plus a per-channel offset kept at object +0x203), clamps to
  [-100, +24] dB, and sends 0x906. The same output gains carry the AVC speed
  factor (`processSpeedEvent`: speed × a config factor → `gain_services`).
  Channels 4-9 aren't driven by setFade.
- **Main mute** (0x913 handler 0x129908): gain words at `1037 + ch`, 10
  channels, written the same way; 0x914 sets one of the discrete ramp times
  listed in audioCtrlSvc.cfg `mute_ramp_rate`.

### 7.2 DSP sample rate: 48 kHz

The tone filters are designed at runtime from the profile's tag-0x61
records (56 bytes, the exact size `fixed_tone_load_eq` copies; it checks for
0x38). Each holds `tan(π·f/fs)` in Q1.31 at +0x10 and Q in Q2.14 at +0x14.
Across all 850 records in the 183 profiles (41 distinct filters), solving
for f gives a **whole number of hertz for every one at fs = 48000** (66, 75,
110, 1000, 1678, 4746, 5000, 7500 Hz...), and for none at 44100. 96 kHz would
also give whole numbers, but all even (75 → 150, 83 → 166), which is
implausible for hand-picked frequencies. **The DSP runs at 48 kHz**, as
`HBAS_DSP_FS` assumed.

### 7.3 The 7 tone slots are not free in stock

Tag-0x61 record layout (big-endian): +0 control, +4 filter type, +0xC
**slot 0-6** (→ biquad 222 + 5·slot), +0x10 tan(π f/fs), +0x14 Q, then 34
bytes of per-type data (gain steps and breakpoints). The control says what
drives the filter (from `fixed_tone_update_filters` 0x128764 and `setTone`):

| Control | Driven by | Seen in profiles |
|---|---|---|
| 1 | **bass knob** (0-16) | slot 0-1, low shelf 66-124 Hz |
| 4 | **treble knob** (0-16) | slot 1-2, high shelf 5.0-6.0 kHz |
| 8 | **volume step** (loudness): field +0xB = volume step - 1, set whenever MEDIA→SPKRS volume changes (0x10d274) | slots 0, 2-6: 60-200 Hz, 1-2.5 kHz, 4.7-7.5 kHz, plus type-8 gain stages |
| 16 | **vehicle speed** (breakpoints 25/50/75/100) | slot 6: a speed-dependent 124 Hz boost |

A typical speaker profile (`02_ON.bin`) uses all 7: slot 0 a gain stage,
1 bass, 2 treble, 3-5 loudness, 6 speed. So these slots carry Harley's
**loudness contour** (bass and treble that change with the volume step; at
high volume bass is pulled back) and speed compensation, not just
bass/treble. **A 7-band user EQ written to all 7 slots replaces that.** The
per-speaker factory EQ (tag 0x36 bank, layout table 11) is a separate set
and is unaffected.

**Bass/treble step 8 is flat** (verified by running the stock design code,
sec. 7.5): steps 0-16 cover roughly -6..+2 dB of bass shelf and -9..+9 dB of
treble shelf at 60 Hz / 10 kHz on `02_ON.bin`.

### 7.5 Harley preset: running Harley's own filter design

`tools/eq-voicing/harley_tone.py` loads stock audioCtrlSvc into Unicorn and
calls its `fixed_tone_load_eq` (0x1288a8) and `fixed_tone_update_filters`
(0x128764) on a profile's tag-0x61 records, so the coefficients are exactly
what the stock radio computes. `gen_harley_eq.py` does this for every
`<cfg>_<ON|OFF>.bin` and `HS_<ON|OFF>.bin` (54 bike configurations plus
headsets), at each of the 18 volume steps, with bass/treble at 8 and speed
0, and fits the curve (relative to 1 kHz) with the HogTiedOS 7-band EQ.
Fit error: median 0.4 dB RMS, 90% under 0.7 dB, worst 2.0 dB (2-speaker
bikes at the lowest volumes, where Harley asks for ~+15 dB of bass and the
sliders stop at +10). Only the fitted integers are committed
(`libhbas/src/harley_eq_table.c`); a test regenerates the table from the
firmware and checks it is identical.

In HogTiedOS the **Harley** EQ preset uses that table for the detected bike
and follows the volume step, engine on/off and speakers vs headset, like the
stock radio. **Flat** stays the default. Configurations with no factory
profile (e.g. 136-139, which ship only a `.conf`) give flat, as stock falls
back to flat. Not reproduced: the speed-dependent boost (slot 6).

### 7.4 Still unknown after this pass

- The DSP's initial state after power-up and how its program is provided
  (no program download in audioCtrlSvc; bench item 1).
- The per-channel fade offset (+0x203) and its source; output-gain channels
  4-9 and mute channels: what each one is.
- Tag 0x61 per-type data layout (the stock code is run instead of decoded,
  sec. 7.5).
