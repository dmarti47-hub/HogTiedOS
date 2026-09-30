# ifs-pack: package Linux so the stock IPL boots it

`mkifs.py` wraps a zImage and DTB into an image the stock Boom! Box IPL loads
from an IFS slot, and validates images offline. `shim.S` is the tiny startup
code the IPL jumps to. **Nothing here writes to hardware.**

```sh
mkifs.py pack --shim shim.bin --kernel zImage --dtb hogtied-boombox.dtb -o hogtied.ifs
mkifs.py verify hogtied.ifs            # also works on the stock completeifs image
```

Buildroot runs both automatically (`board/boombox/post-image.sh`) and fails
the build if validation fails.

## Boot flow (IPL behavior measured with tools/ipl-emu)

1. The IPL reads `stored_size` bytes of the slot to **0x84000000** and checks
   that both regions sum to zero.
2. It copies the startup region (header + shim) to **0x80100000**, sets
   header+0x28 to `0x84000000 + startup_size`, and jumps to the shim with the
   MMU and caches off, in SVC mode, with interrupts masked.
3. The shim prints `HogTiedOS shim: starting Linux` on UART3. It reads
   (never writes) EMIF4 SDRAM_CONFIG exactly like stock startup; if ROWSIZE
   is 5, it rewrites the DTB's `/memory@80000000` size cell from 256 to
   512 MiB. Then it enters the zImage with `r0=0, r1=0xffffffff, r2=DTB`.

## Image layout

| Region | Contents | Rule |
|---|---|---|
| startup `[0, startup_size)` | 0x100 QNX startup header, shim, padding, filler word | sums to 0; size is a 4 KiB multiple so the payload is page-aligned |
| payload `[startup_size, stored_size)` | table (`HTLX`, version 2, kernel/DTB offsets and sizes, DTB offset of the memory size cell), zImage @ +0x1000, DTB page-aligned, filler word | sums to 0 |

Limits: `stored_size` up to 42 MiB (the IPL's check). It must also fit in
256 MiB of RAM from 0x84000000.

## Testing

`test_mkifs.py` packs a real build and runs it through the **stock IPL** in
the emulator. It checks that the IPL accepts it, that the shim reaches the
exact zImage bytes with the exact DTB in r2 (8-byte aligned), that the MMU
and caches are still off, that it also boots from slot 2, and that a
corrupted image is rejected by both the validator and the IPL. It simulates
both RAM straps: 256 MiB units get the DTB unchanged, 512 MiB units get only
the size cell changed. After the handoff the shim writes nothing but UART3.

```sh
HOGTIED_SHIM=.../shim.bin HOGTIED_ZIMAGE=output/images/zImage \
HOGTIED_DTB=output/images/hogtied-boombox.dtb \
    python3 -m unittest tools/ifs-pack/test_mkifs.py -v
```

What this can't show: whether the kernel then runs on the DRA526. That needs
hardware.
