# HogTiedOS userspace

| Directory | What |
|---|---|
| `libhbas/` | C decoder for the bike's CAN messages, field for field from Harley's own `vehicleCAN.lua` (`docs/findings/CROSS_CHECKS.md` sec. 11). No UI or I/O. |
| `hogtied-ui/` | The main screen, 400x240, LVGL 9.2.2: speed, gear, rpm, warning lights, clock, ambient temperature, tire data. |
| `iocd/` | `hbas-iocd`: the IOC link on the unit. Answers the power keep-alive, handles shutdown, and forwards bike CAN frames to `vcan0` for the UI. |

Both are built into the image by Buildroot (`buildroot-external/package/hogtied-ui`).

![Demo ride rendered offline](../docs/screenshots/ui-demo.png)

![Audio page](../docs/screenshots/ui-audio.png)

![EQ page](../docs/screenshots/ui-eq.png)

## Running the screen on your PC

One-time setup (Ubuntu / Debian / Pop!_OS):

```sh
sudo apt install build-essential cmake pkg-config curl libsdl2-dev
```

Then, from the repository folder:

```sh
software/run-ui-on-pc.sh
```

The first run downloads LVGL and builds (a minute or two); later runs start
right away. A window opens with the screen at 2x size, playing a demo ride.

| Key | Does |
|---|---|
| Left / Right arrows | change page (Dash, Audio, EQ, Tires, System) |
| Up / Down | Audio: pick a setting. EQ (7 bands, the only tone control): change preset, or a band's level while adjusting |
| Enter | start / finish adjusting (Audio: Left/Right change the value; EQ: Left/Right pick the band) |
| Esc | back to Dash (or finish adjusting) |
| close the window, or Ctrl+C in the terminal | quit |

Other data sources: `software/run-ui-on-pc.sh --replay FILE` (lines like
`541#0BB803E800000300`, candump -L style) or `--can vcan0` (live Linux
SocketCAN). **Never run `--demo` on a bike**: it shows fake data.

## Developer builds

```sh
# decoder/protocol unit tests
cmake -S software/libhbas -B build/hbas && cmake --build build/hbas && (cd build/hbas && ctest)

# offline screenshots (no display needed): writes shot-*.bmp from the demo ride
cmake -S software/hogtied-ui -B build/ui -DLVGL_DIR=$PWD/build/lvgl-9.2.2 && cmake --build build/ui
build/ui/hogtied-ui --snapshot shot
```

Backends: `-DHOGTIED_SDL=ON` desktop window, `-DHOGTIED_FBDEV=ON` Linux
framebuffer (what the head unit uses), neither = snapshot only. On the unit,
buttons arrive from hbas-iocd as the `hbas-buttons` input device;
`--stdin-keys` also accepts a/d/w/s/Enter/q from a terminal.

## What's real and what isn't yet

- **Real:** decoding. Every field layout comes from the stock decoder, and the
  tests cover each message, error values and short frames.
- **Written, not hardware-tested:** the IOC link (`libhbas/ioc.c` + `iocd`).
  The protocol is verified against stock `dev-ipc` and the stock Lua services
  (CROSS_CHECKS sec. 12), and the tests run against a simulated IOC. On the
  unit: `hbas-iocd` → `vcan0` → `hogtied-ui --can vcan0`. Unknown until
  hardware: the REQ/ACK edge polarity (both edges are used).
- **Audio: settings only, nothing reaches the DSP yet.** The Audio page uses
  the stock ranges and step-to-dB tables, and the EQ profile follows the
  engine; changes are logged. Which DSP writes they become is what the bench
  capture is for (`docs/findings/AUDIO.md`). `--speakers 2` hides fade.
- **Shown raw on purpose:** gear numbers, and tire pressure/temperature. The
  stock code doesn't define their meanings or units, so the UI doesn't
  guess.
- **Buttons, written, not hardware-tested:** channel-3 handlebar and
  front-panel buttons are decoded with the stock key map (CROSS_CHECKS
  sec. 13) and appear as the `hbas-buttons` keyboard. The UI pages with
  either handlebar's left/right, and HOME goes back. Run `hbas-iocd -v` to
  log raw button bytes and confirm layout A on the first hardware run.
