# GPS: stock setup, and what HogTiedOS does

Sources: stock `etc/boot.sh` (`start_ndr`), `etc/system/config/gpio-harley.conf`,
`usr/bin/vdev-flexgps` strings, the earlier `bt_wifi_gps_map.json`. Nothing
here has been observed on hardware.

## 1. Stock

- **Receiver:** u-blox 5 or 6 (flexgps detects "UBX-G5xxx" / "UBX-G60xx").
- **Port:** UART2, 0x4806C000, IRQ 73 (QNX `/dev/ser3`; Linux `ttyS1`).
  `devc-seromap -E -F -u3 -b9600` (no flow control).
- **Baud:** boot.sh opens 9600 and immediately sends
  `$PUBX,41,1,0007,0003,57600,0*2B` (u-blox "set port": input UBX+NMEA+RTCM,
  output UBX+NMEA, 57600 baud; checksum verified). flexgps is then started
  with `-b9600 -C115200` and sends its own `$PUBX,41,1,0003,0003,%d,0` with a
  rate of its choosing (115200 is its firmware-update rate), so the rate the
  receiver ends up at isn't fixed by the scripts alone.
- **Reset:** GPIO112 (gpio4 line 16), `output, GPSReset, 112, invert No,
  init yes, initval 0`: driven low at boot, the receiver runs. flexgps can
  pulse it (`-P/dev/gpio/GPSReset`).
- **`g5.cfg`:** a 1725-byte encoded configuration blob flexgps sends; not
  decoded (entropy 7.88 bits/byte). Not needed for NMEA output.
- **Dead reckoning:** `ndr -e=DR -ch5=/dev/ipc/ch5` (Navteq/NNG "navigation
  dead reckoning") reads IOC IPC channel 5, presumably wheel speed / gyro for
  tunnels. Not examined yet.

## 2. HogTiedOS

- Device tree: UART2 on; GPIO112 held low as a gpio-hog, as stock does.
- **hbas-gpsd** (`software/gpsd`): opens `/dev/ttyS1` at 9600, sends stock's
  `$PUBX,41,...,57600`, switches to 57600 and waits for sentences with good
  checksums; if none come it tries 9600, 115200, 38400, 19200, 4800 in turn,
  and hunts again if the receiver is quiet for 5 s. Parses RMC, GGA, GSA,
  GSV, VTG (`libhbas/nmea.c`, any talker ID). Serves the fix once a second
  on `/run/hbas/gps.sock` (`libhbas/gpsproto.h`). With `--set-clock` (on the
  unit) a valid fix sets the system clock when it's more than 2 s off.
- **GPS week-number rollover:** u-blox 5 era firmware can report dates
  1024 weeks early since 2019-04-06; dates before 2025 are moved forward by
  1024 weeks before setting the clock.
- **GPS page** in the UI: speed (bike's own units), heading, position,
  altitude, HDOP, UTC, satellite signal bars.
- PC: `run-ui-on-pc.sh` plays `software/gpsd/demo/ride.nmea`, a made-up
  3-minute loop (`make_ride.py`).

## 3. Open

- The receiver's actual baud after power-up on this unit (handled by
  hunting, but worth logging on first boot).
- Whether the antenna needs a supply enabled (no such GPIO seen in stock).
- IOC channel 5 (dead-reckoning sensors).
- Maps / navigation: not started.
