# Radio: AM/FM/WB tuner

Target: the Boom! Box 6.5GT broadcast radio. Everything here is from static
analysis of the stock QNX firmware (`firmware/`, gitignored); nothing has
been confirmed on a live unit.

## 1. Hardware (from the stock firmware)

- **Tuner: Silabs Si4763** (AM/FM/WB), plus an **Si4749 HD Radio**
  coprocessor. `firmware/.../mnt/persistence/config/tuner.cfg` says verbatim:
  *"This is a config file for the SiLabs Si4763 tuner running on Harley ISYS
  DV3 hardware."* `dev-tuner` has `si47xx_device_init`, RDS/RT+, HD Radio
  (`hdStatusFSM`), DARC/VICS (Japan, over SPI — not NA).
- **Bus: OMAP I2C1 (0x48070000)**, exposed by QNX as `/dev/i2c1`. Stock
  `etc/start_tuner.sh`:
  ```
  i2c-omap35xx -a1 -i56 -p0x48070000 --u1
  dev-tuner -E /dev/i2c1 -D 0x4763 -i 0x62 -t 1 -D 0x4749 -i 0x11 -t 3 \
            -C .../tuner.cfg -G .../tunerData -p 21 ...
  ```
  So **Si4763 at I2C address 0x62** (type 1) and **Si4749 at 0x11** (type 3).
- Audio from the tuner reaches the audio path via `/dev/snd` (I2S/McBSP);
  the exact DSP mixer input isn't yet pinned (see AUDIO.md).
- **Unconfirmed:** the tuner **reset GPIO**, and whether the Si4763 loads its
  firmware from an on-board SPI flash (host just sends commands) or must be
  loaded by the host. The launch command references no firmware `.bin`, which
  suggests the chip boots its own firmware — but this needs hardware to
  confirm. `-p 21` is unexplained (priority or a GPIO).

## 2. Band plan (NA, region 1), from tunerserver/tunerlaunch.lua

| Band | Range | Step | Default |
|---|---|---|---|
| AM | 530-1700 kHz | 10 kHz | 530 |
| FM | 87.7-107.9 MHz | 200 kHz | 105.7 |
| WB | channels 1-7 (NOAA 162.400-162.550 MHz) | 1 | 1 |

Other regions (ECE/Asia/etc.) exist in the Lua; HogTiedOS targets NA.
Seek stop thresholds (RSSI/SNR/multipath/offset) are in `tuner.cfg`.

## 3. What HogTiedOS implements

- **libhbas `tunerproto.{h,c}`** (tested): the model + band plan + the line
  protocol between the UI and `hbas-tunerd` (`tuner ...` state, `rds ...`,
  and `band`/`tune`/`seek`/`power` commands).
- **`software/tunerd` (hbas-tunerd)**: serves the state on
  `/run/hbas/tuner.sock`. Two backends:
  - **demo** (PC): a set of simulated stations with RDS, so the UI works
    with no radio. `run-ui-on-pc.sh` runs this.
  - **Si4763 over I2C** (hardware): opens `/dev/i2c-1` and drives the chip
    with the public Si476x command set. **Not yet active** — it prints a
    notice and falls back to demo until the bring-up above is confirmed on
    hardware. Nothing is written to the chip without that.
- **UI** (Music page -> Radio): band (FM/AM/WB), frequency, tune and seek,
  signal/stereo, RDS station and radio text, and six presets (tap to recall,
  hold to save, kept in `radio.conf` next to the settings).
- **Device tree**: `&i2c1` enabled (0x48070000). The Si4763 node is not
  declared with a kernel driver; `hbas-tunerd` opens the bus directly.

## 4. Out of scope / open

- **HD Radio** (Si4749): needs proprietary Silabs/Harley firmware; not
  committable and far more complex. Analog AM/FM/WB only.
- The Si4763 **firmware-load question** and **reset GPIO** must be answered
  on hardware before the I2C backend can be switched on.
- Tuner **audio routing** into the DSP (which mixer input) — tied to the DSP
  SPI writer, which also doesn't exist yet.
