# Bluetooth: stock hardware path, and what HogTiedOS does

Sources: stock `etc/bluetooth.sh`, `etc/wicomeConf/wicome.cfg`,
`mnt/persistence/wicome/wicomeBin/dev-sdio` and
`wicomeLib/libpal_bt_csr.so` (both unstripped), disassembled with
`tools/re/qnxdis.py`. Nothing here has been observed on hardware.

## 1. The chip and how it's connected

- **CSR BlueCore ROM chip on SDIO (MMC3), not a UART.** `bluetooth.sh` starts
  `dev-sdio ioport=0x480ad000,irq=94` (MMC3). That driver ("SDIO interface to
  bluetooth chip") drives the MMC3 controller itself and offers the Bluetooth
  stack a fake serial port, `/dev/ser2`, plus `/dev/BTReset`. wicome.cfg then
  uses `UART_DEV = /dev/ser2`, `HCI_TRAN = H4`, `BT_CHIP = CSR_ROM`.
- The earlier finding of a Marvell 88W8688 came from a Wi-Fi driver
  (`devnp-mv8688uap-sta2x11.so`) that ships but is never started; the
  commented-out second `dev-sdio` line (MMC2) is the only other Bluetooth path.
- **SDIO protocol = standard Bluetooth Type-A** on function 1. Decoded CMD52
  arguments from `dev-sdio`:

  | Write | Meaning |
  |---|---|
  | CCCR 0x07 = 0x82 | 4-bit bus, card-detect pull-up off |
  | CCCR 0xF0 = 0x11 | vendor-specific register (not in the SDIO spec) |
  | CCCR 0x12 = 0x03 | power control |
  | CCCR 0x02 = 0x02 | enable function 1 |
  | CCCR 0x04 = 0x03 | interrupt enable: master + function 1 |
  | F1 0x14 = 0x01 | Type-A: enable "packet ready" interrupt |
  | F1 0x13 = 0x01 | Type-A: clear interrupt |
  | F1 0x00, 4 bytes | Type-A: read packet header (length + service ID) |
  | F1 0x10 = 0x00 | Type-A: read acknowledge |

  That is the register set Linux's generic `btsdio` driver uses, so
  HogTiedOS uses `CONFIG_BT_HCIBTSDIO`. Unconfirmed: whether the vendor
  CCCR 0xF0 write is needed (btsdio doesn't do it).
- **No power/reset GPIO**: `dev-sdio` maps only the MMC3 registers.
  `/dev/BTReset` is an in-band reset: writing it makes `dev-sdio` re-run its
  SDIO init (`SendResetSequence`, `ResendInitSequence`). It also re-runs the
  init when it sees the stack's CSR warm-reset command go past
  (`wasAResetCommand`), and it rewrites the stack's BCSP sync pattern
  (`c0 40 41 00 7e da dc ed ed a9 7a c0`) into an HCI Read_Buffer_Size.

## 2. CSR chip setup (libpal_bt_csr.so)

Stock sends CSR BCCMD commands (HCI vendor command 0xFC00, channel byte
0xC2) at startup. Every one is stored in the library as complete bytes,
with a 16-byte list entry per command (opcode, length, data pointer, and a
response check: vendor event or HCI command complete).

- `BCCMD_GET_BUILD_ID` (varid 0x2819) picks the patch set: there are ROM
  patch sets for firmware builds **0x12E9**, **0x07A6** and **0x0C5C**
  (e.g. `PSKEY_PATCH152_0x12E9`, `PSKEY_PMALLOC_SIZES_0x12E9`,
  `PSKEY_TEMPERATURE_VS_DELTA_*_0x12E9`), each a PS-key write.
- **Project PS keys** from wicome.cfg; the name → key table
  (`projectSettings`) gives the numbers, which match CSR's public PSKEY ids:

  | wicome.cfg | PS key | Value set |
  |---|---|---|
  | PROJ_PSKEY_ANA_FREQ | 0x01FE | 0x6590 = 26000 kHz crystal |
  | PROJ_PSKEY_LC_MAX_TX_POWER | 0x0017 | 4 |
  | PROJ_PSKEY_LC_DEFAULT_TX_POWER | 0x0021 | 4 |
  | PROJ_PSKEY_MAX_TX_POWER_NO_RSSI | 0x002D | 4 |
  | PROJ_PSKEY_ENHANCED_POWER_TABLE | 0x0031 | 35-word table |
  | PROJ_PSKEY_PCM_CONFIG32 | 0x01B3 | 0x0800, 0x0002 |
  | PROJ_PSKEY_PCM_FORMAT | 0x01B6 | 0x006C |
  | PROJ_PSKEY_PCM_ALWAYS_ENABLE | 0x01C9 | 1 |
  | PROJ_PSKEY_HOSTIO_MAP_SCO_PCM | 0x01AB | 1 |
  | PROJ_PSKEY_AMUX_AIO1 | 0x022B | 0x0300 |
  | PROJ_PSKEY_PCM_LOW_JITTER_CONFIG | 0x01BA | (not set in this cfg) |

  The PCM keys route phone-call (SCO) audio to the chip's PCM pins
  (McBSP2, clocked by `BTClockSet`); music doesn't use them.
- **Bluetooth address** from the EEPROM (`BT_ADDR_FILE =
  /dev/mmap/eeprom/BT_ID`), written as PS key 0x0001 (`BCCMD_PSKEY_BD_ADDR`;
  its template holds CSR's default 00:02:5B:00:A5:A5).
- Then `BCCMD_WARM_RESET` (varid 0x4002) so the chip applies the keys.

## 3. Music audio path

wicome decodes A2DP on the main processor (`BSS_A2DP.InternalSbcDecoder`,
sinks of type `MEDIASTREAM`, 44.1/48 kHz) and plays it into the normal media
path: McBSP4 → DSP → `main_mixer1` MEDIA inputs (AUDIO.md sec. 7.1). So
phone music needs the same McBSP4 output as any other source.

## 4. What HogTiedOS does

- Device tree: MMC3 `bus-width = <4>`, non-removable.
- Kernel: `CONFIG_BT`, `CONFIG_BT_BREDR`, `CONFIG_BT_HCIBTSDIO`, plus the
  crypto BlueZ needs for pairing.
- BlueZ (`bluetoothd`, name "HogTiedOS", car-audio class), bluez-alsa as the
  A2DP sink, and **hbas-btd**: phone, track and play state for the Media
  page, play/pause/next/previous/stop through AVRCP, and on-screen pairing
  confirmation (6-digit code, OK / Back). Pairing keys are saved to the eMMC
  (`hogtied/bluetooth.tar`, written only during the save) and restored at
  boot, because the root filesystem is in RAM.

## 5. Not done yet (needed before music plays on the unit)

1. **CSR init on Linux.** Replay stock's BCCMD sequence for the chip's
   build ID: patches, the project keys above (crystal frequency and TX
   power matter most), the EEPROM address, warm reset. The warm reset
   restarts the chip's SDIO function, which `dev-sdio` handles by re-running
   its init; on Linux that needs a btsdio unbind/rebind or MMC rescan, which
   can only be tried on the unit. The patch and command bytes are Harley/CSR
   data, so they'd be extracted from the user's firmware at build time, not
   committed.
2. **McBSP4 audio output** (ALSA device into the DSP): the frame format,
   clocking and slot layout of `deva-ctrl-j3-isys_dsp.so` aren't decoded yet.
   Until then bluez-alsa has nowhere to play.
3. EEPROM layout of `BT_ID` (offset in the I2C3 EEPROM).
