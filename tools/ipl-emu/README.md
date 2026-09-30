# ipl-emu: run the stock IPL on a PC

Runs the unmodified stock Boom! Box IPL in the Unicorn CPU emulator against
simulated hardware, to check whether the real IPL would accept an IFS image.
**Nothing here touches hardware.** The stock firmware isn't in this repo; point
`--ipl` at your copy (by default the tests look under the gitignored
`firmware/`).

```sh
python3 -m venv .venv && .venv/bin/pip install -r tools/requirements.txt
.venv/bin/python tools/ipl-emu/ipl_emu.py --ipl <ipl.bin> --ifs <image.ifs> --slot 0
.venv/bin/python -m unittest tools/ipl-emu/test_ipl_emu.py -v
```

Output ends with either `HANDOFF to <addr>` (plus the CPU state) or
`no handoff`, followed by the IPL's own console log.

## What's simulated
- UART3 console (captured)
- clock/PLL ready-polls (answered "ready") and the IPL's delay counter
- x16 NAND on the GPMC: read ID, cached sequential reads, prefetch FIFO.
  Images are laid out with the spare-area page counter the IPL checks.
- boot-state EEPROM behind OMAP I2C3, with reads and writes logged

## What's not
- **ECC bytes:** the BCH result registers always report no errors. The real
  ECC is written by stock `update_nand_teb`, not by our packer.
- bad blocks, timing, the DRAM test, real clock lock

A pass means the stock IPL's own code accepts the image's layout, header and
checksums and hands off in a known CPU state. It is not proof the image boots
on hardware.
