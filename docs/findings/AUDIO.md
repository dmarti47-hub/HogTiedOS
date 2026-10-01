# Audio: stock behaviour, what HogTiedOS implements, and the bench capture

Sources: stock `PRE_CAL/cfg/audioMgr.cfg` and `audioCtrlSvc.cfg` (JSON with
Harley's own descriptions), `usr/share/lua/service/eqService/eqService.lua`,
`bin/audioCtrlSvc` (disassembled with `tools/re/qnxdis.py`), and
`dsp_layout_map.json`. Nothing here has been observed on hardware.

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

**Open:** which tone step means "flat". The curves map step N to N dB, but
nothing says whether the HMI centre (8) is flat or 0 is. HogTiedOS uses the
centre for now.

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

## 3. Stock DSP operations (audioCtrlSvc → io-audio driver devctl)

Each devctl call's error message names the operation, and the command number
is loaded right before the call:

| Operation | devctl | dsp_layout_map table index | Used for (from the code) |
|---|---|---|---|
| `DSP_REGISTER_IO_HANDLE` | 0x901 | - | setup |
| `DSP_LOAD_EQ` | 0x905 | features 8, 11, 13, 20, 22 | factory EQ profile |
| `DSP_SET_OUTPUT_GAIN` | 0x906 | 12 | volume / fade (`gain_services_calc_gain`, `setFade(frontGain, rearGain)`) |
| `DSP_SET_TONE` | 0x907 | 4 (biquad bank) | bass/treble knobs (`setTone(trebleKnob, bassKnob)`), filter updates after gain changes |
| `DSP_SET_INPUT_GAIN` | 0x90C | 2 | source gain matching |
| `DSP_SET_MAIN_AUDIO_MUTE` | 0x913 | 14 | mute |
| `DSP_SET_STATIC_BIQUAD_CHAIN` | 0x925 | 19 | fixed filters |
| `DSP_SET_DEVICE`, `DSP_SET_CLKLESS_XBAR_MIXER` | not resolved | | routing / mixer |

Tone is therefore **computed filter coefficients**, sent through the biquad
safe-load path (0x907): the glitch-free real-time path `PROJECT_DECISIONS.md`
planned around. The stock code also has an **AVC level** adjustment (automatic
volume control, i.e. speed-sensitive volume) and a built-in fallback
`Harley_FlatEQ_withToneLoudness.EQF`, so loudness compensation exists too.

## 4. What HogTiedOS implements now

- `software/libhbas/audio.c`: settings model with the stock ranges and tables,
  per-output volume, fade only on 4-speaker bikes and only to speakers, and
  the stock EQ file naming (tested).
- `hogtied-ui` Audio page: Volume, Fade, Output; the factory EQ profile
  follows the engine-running flag from the bike. **Bass/treble are dropped
  on purpose**: the user EQ below replaces them.
- `hogtied-ui` EQ page + `software/libhbas/eq.c`: a **7-band** graphic EQ
  (63, 160, 400 Hz, 1, 2.5, 6.3, 16 kHz; ±10 dB; Q 1.05 for the ~1.33-octave
  spacing) with presets, made of RBJ peaking biquads. They're encoded in the
  verified DSP format (sec. 6) and packed into one 35-word safe-load. It's
  the only tone control, and it **assumes all 7 biquads of the set stock's
  bass/treble (`DSP_SET_TONE`, 0x907) write to are free** for it.
- **Backend: logging only.** Every change produces the dB values a DSP backend
  would apply, but nothing is sent: the parameter-level writes behind
  0x906/0x907/0x913 aren't known yet.

## 5. Bench capture checklist

Goal: turn each setting into exact SPI writes, so a Linux DSP backend can
reproduce them. Everything else (ranges, curves, profile selection) is
already known from the firmware.

**Setup:** stock firmware, unit on the bench, logic analyzer on the DSP SPI bus
(McSPI2: CLK, SIMO, SOMI, CS0; 6 MHz, mode 0, 8-bit, CS held for the whole
transfer) plus GPIO170 (power/reset) and GPIO102 (DSP completion input).
Pads are known (IPL pad table: 0x21D6/0x21D8/0x21DA/0x21DC), physical test
points aren't. Record with timestamps, and note each HMI action as it's made.

| # | Do this on the stock HMI | Expect to see | Gives us |
|---|---|---|---|
| 1 | Power up, wait for audio | init writes, GPIO170 / GPIO102 timing | DSP boot/reset sequence (an open item) |
| 2 | Volume 0 → 17, one step at a time | 0x906 writes (table 12), maybe 0x907 | volume parameter address(es) and value encoding per step |
| 3 | Bass 0 → 16, then treble 0 → 16 | 0x907 safe-loads to biquads at 222 + 5i | coefficients per step, so we can match or tabulate them; also shows which step is flat |
| 4 | Fade full rear → full front | 0x906 front/rear gains | fade encoding |
| 5 | Mute / unmute | 0x913 (table 14) | mute register |
| 6 | Start / stop engine (or replay CAN) | 0x905 profile load | confirms automatic EQ switching |
| 7 | Speakers ↔ headset | 0x905 HS profile, routing writes | output routing writes |
| 8 | Change source (media → phone → nav) | 0x90C, mixer writes | input gain / mixer |
| 9 | Ride or replay speed changes | AVC-related writes | speed-sensitive volume curve |

Items 2-5 are what the Audio page needs. For the EQ page, item 3 is the
key check on the 7-free-slots assumption: it shows which of the 7 biquads
stock's bass/treble actually rewrite, and whether the factory profile (0x905,
tag 0x35 slot map) also puts filters there, and the DSP sample rate (needed to compute coefficients) can be checked
by capturing a known bass/treble step and comparing its coefficients with
the RBJ formulas at 44.1 vs 48 kHz. Static analysis of `audioCtrlSvc`'s
`gain_services_*` and `fixed_tone_*` routines could recover some of it before
any capture; the capture then confirms it.

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
DSP address 222, stride 5, at most 7 per set).

Still open: the DSP **sample rate** (coefficients depend on it; HogTiedOS
assumes 48 kHz), and **which biquad slots** a user EQ can own without
fighting the factory profile (tag 0x35 maps profile filters to slots).
