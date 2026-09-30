#!/usr/bin/env python3
"""Run the stock Boom! Box IPL in Unicorn against simulated hardware.

Purpose: check offline whether the *real* stock IPL would accept a given
IFS image, without touching any hardware. The IPL binary comes from the
stock firmware (not in this repo; see --ipl).

Models: UART3 console, clock/PLL ready-polls, the IPL's delay counter,
x16 NAND behind the GPMC (ID, cached sequential reads, prefetch FIFO,
zero BCH syndromes), and the boot-state EEPROM behind OMAP I2C3.

NOT modeled: ECC bytes themselves (the BCH result registers always report
"no errors"; update_nand_teb writes the real ECC, not our packer), bad
blocks, timing. A pass here means the stock IPL's own code accepts the
image layout, header and checksums, and hands off. It is not a hardware test.
"""
import argparse
import struct
import sys

from unicorn import (Uc, UcError, UC_ARCH_ARM, UC_MODE_ARM, UC_HOOK_CODE,
                     UC_HOOK_MEM_READ, UC_HOOK_MEM_WRITE,
                     UC_HOOK_MEM_UNMAPPED, UC_HOOK_INTR)
from unicorn.arm_const import (UC_ARM_REG_PC, UC_ARM_REG_SP, UC_ARM_REG_CPSR,
                               UC_ARM_REG_R0, UC_ARM_REG_CP_REG,
                               UC_CPU_ARM_CORTEX_A8)
from capstone import Cs, CS_ARCH_ARM, CS_MODE_ARM

SRAM_BASE, SRAM_SIZE = 0x40200000, 0x10000
DRAM_BASE, DRAM_SIZE = 0x80000000, 0x10000000        # 256 MiB (both variants have >= this)
UART3 = 0x49020000                                    # debug console (confirmed)
UART_LSR = UART3 + 0x14
# Free-running counter the IPL's delay loop (0x40200E90) spins on; the loop
# masks it and handles wrap, so any monotonic tick works.
DELAY_COUNTER = 0x49030028
IPL_LOAD_ADDR = 0x84000000   # the IPL reads the whole image here (measured)

# GPMC (confirmed base 0x6E000000), CS0 NAND registers
GPMC = 0x6E000000
GPMC_STATUS = GPMC + 0x54            # bit 8 = WAIT0 (NAND ready)
NAND_CMD, NAND_ADDR, NAND_DATA = GPMC + 0x7C, GPMC + 0x80, GPMC + 0x84
GPMC_PREFETCH_STATUS = GPMC + 0x1F0  # [30:24] FIFO byte count
GPMC_BCH_RESULTS = range(GPMC + 0x240, GPMC + 0x270)
PREFETCH_FIFO = 0x08000000           # CS0 window; IPL reads page data here
# NAND geometry (confirmed): 2048 + 64 byte pages, 64 pages/block. The IPL
# addresses the spare area at column 0x400, i.e. a x16 part (ID 0xCA/0xBA).
PAGE, OOB, PPB = 2048, 64, 64
NAND_ID = bytes([0x2C, 0xCA, 0x90, 0xD5, 0x06])   # Micron 2Gb x16-class; IPL checks byte[1]

# EEPROM on OMAP I2C3 (confirmed: QNX /dev/i2c2 = 0x48060000), addr 0x50
I2C3 = 0x48060000
I2C_STAT, I2C_SYSS, I2C_CNT, I2C_DATA, I2C_CON, I2C_SA = (
    I2C3 + 0x08, I2C3 + 0x10, I2C3 + 0x18, I2C3 + 0x1C, I2C3 + 0x24, I2C3 + 0x2C)


class Nand:
    """x16 NAND with the cached-sequential read commands the IPL uses."""

    def __init__(self):
        self.pages = {}        # page number -> 2112 bytes
        self.addr = []
        self.out = b''
        self.pos = 0
        self.pending = None    # page loaded by 0x30, streamed by 0x31/0x3F
        self.mode = None       # 'id' / 'status' (byte cycles) or 'page' (x16 words)
        self.log = []

    def page(self, n):
        return self.pages.get(n, b'\xff' * (PAGE + OOB))

    def command(self, c):
        if c in (0x00, 0x90, 0x70, 0xFF):
            self.addr = []
        if c == 0xFF:
            self.out, self.pos, self.mode = b'', 0, None
        elif c == 0x90:
            self.out, self.pos, self.mode = NAND_ID, 0, 'id'
        elif c == 0x70:
            self.out, self.pos, self.mode = b'\xe0', 0, 'status'   # ready, not WP
        elif c == 0x00:
            pass
        elif c == 0x30:
            self.mode = 'page'
            col = (self.addr[0] | self.addr[1] << 8) * 2 if len(self.addr) >= 2 else 0
            row = sum(b << (8 * i) for i, b in enumerate(self.addr[2:5]))
            self.pending = row
            self.out, self.pos = self.page(row), col
            self.log.append(('read', row, col))
        elif c == 0x31:
            self.mode = 'page'
            self.out, self.pos = self.page(self.pending), 0
            self.pending += 1
        elif c == 0x3F:
            self.mode = 'page'
            self.out, self.pos = self.page(self.pending), 0
        else:
            self.log.append(('unhandled-cmd', hex(c)))

    def address(self, a):
        self.addr.append(a & 0xff)

    def read_id_mode(self):
        return self.mode in ('id', 'status')

    def load_image(self, image, start_block):
        """Lay an IFS image out page by page, spare area 0xFF except the
        page sequence number at spare offset +4. The IPL expects 0 on the
        image's first page and +1 on each following page (it increments
        its expected value per page read, 0x40203534)."""
        first = start_block * PPB
        for i in range(0, len(image), PAGE):
            data = image[i:i + PAGE].ljust(PAGE, b'\xff')
            spare = bytearray(b'\xff' * OOB)
            spare[4:8] = struct.pack('<I', i // PAGE)
            self.pages[first + i // PAGE] = data + bytes(spare)

    def read(self, n):
        chunk = self.out[self.pos:self.pos + n]
        self.pos += n
        return chunk.ljust(n, b'\xff')


class Eeprom:
    """24Cxx-style EEPROM behind the OMAP I2C controller (2-byte offsets)."""

    def __init__(self, image):
        self.mem = bytearray(image)
        self.ptr = 0
        self.tx = []
        self.log = []
        self.writes = []       # (offset, value) the IPL wrote, in order

    def write_byte(self, b):
        self.tx.append(b & 0xff)
        if len(self.tx) == 2:
            self.ptr = (self.tx[0] << 8 | self.tx[1]) % len(self.mem)
        elif len(self.tx) > 2:
            self.mem[self.ptr] = b & 0xff
            self.writes.append((self.ptr, b & 0xff))
            self.ptr = (self.ptr + 1) % len(self.mem)

    def start(self, transmit):
        if transmit:
            self.tx = []
        else:
            self.log.append(self.ptr)

    def read_byte(self):
        b = self.mem[self.ptr]
        self.ptr = (self.ptr + 1) % len(self.mem)
        return b


class IplEmu:
    def __init__(self, ipl_file, nand=None, eeprom=None, verbose=False):
        raw = open(ipl_file, 'rb').read()
        size, load = struct.unpack_from('<II', raw, 0)
        assert load == SRAM_BASE, hex(load)
        self.body = raw[8:8 + size]
        self.verbose = verbose
        self.console = bytearray()
        self.ticks = 0
        self.nand = nand or Nand()
        self.eeprom = eeprom
        self.unmapped = []
        self.u = u = Uc(UC_ARCH_ARM, UC_MODE_ARM, cpu=UC_CPU_ARM_CORTEX_A8)
        u.mem_map(SRAM_BASE, SRAM_SIZE)
        u.mem_map(DRAM_BASE, DRAM_SIZE)
        u.mem_write(SRAM_BASE, self.body)
        # Peripheral windows as plain RAM; specific registers get models below.
        for base, sz in [(0x48000000, 0x400000), (0x49000000, 0x100000),
                         (0x6c000000, 0x3000000), (PREFETCH_FIFO, 0x100000)]:
            u.mem_map(base, sz)
        # Hooks only on hardware windows: hooking all of DRAM is far too slow.
        for lo, hi in [(0x48000000, 0x483fffff), (0x49000000, 0x490fffff),
                       (0x6c000000, 0x6effffff), (PREFETCH_FIFO, PREFETCH_FIFO + 0xfffff)]:
            u.hook_add(UC_HOOK_MEM_WRITE, self._on_write, begin=lo, end=hi)
            u.hook_add(UC_HOOK_MEM_READ, self._on_read, begin=lo, end=hi)
        u.hook_add(UC_HOOK_MEM_UNMAPPED, self._on_unmapped)
        u.hook_add(UC_HOOK_INTR, self._on_intr)
        # The boot ROM hands over with SP in on-chip SRAM.
        u.reg_write(UC_ARM_REG_SP, SRAM_BASE + SRAM_SIZE - 0x10)
        # `smc #0` calls go to the ROM's secure monitor (cache/errata
        # maintenance on this GP device). Treat them as no-ops.
        md = Cs(CS_ARCH_ARM, CS_MODE_ARM)
        self.insns = self._sweep(md)
        self.smc = {i.address for i in self.insns if i.mnemonic == 'smc'}
        for off in self.smc:     # the IPL runs from SRAM, then from its DRAM copy
            for base in (SRAM_BASE, DRAM_BASE):
                u.hook_add(UC_HOOK_CODE, self._on_code, begin=base + off, end=base + off)
        self.polls = self._find_polls()
        # Anything executed outside the IPL (SRAM copy / DRAM copy) is the
        # handoff into the loaded image: stop there and record CPU state.
        self.handoff = None
        self.kernel_entry = None
        self.follow = False        # keep running past the handoff (our shim)
        u.hook_add(UC_HOOK_CODE, self._on_handoff, begin=DRAM_BASE + 0x10000,
                   end=DRAM_BASE + DRAM_SIZE - 1)

    def _sweep(self, md):
        """Linear sweep, one word at a time (capstone stops at data words)."""
        out = []
        for off in range(0, len(self.body) - 3, 4):
            out.extend(md.disasm(self.body[off:off + 4], off))
        return out

    def _find_polls(self):
        """Find `ldr; ...; cmp/tst #imm; bne/beq <back>` status-poll loops.

        Returns {code offset of the ldr: value to return} so that clock/PLL
        style ready-waits exit. Same heuristic as the verification kit's
        replay_board_init.py, extended to `tst`. Registers with a real model
        below (UART, NAND, I2C) are never overridden by this.
        """
        ins = self.insns
        at = {i.address: i for i in ins}
        polls = {}
        for i in ins:
            if i.mnemonic != 'ldr' or '[pc' in i.op_str:
                continue
            seq = [at.get(i.address + 4 * j) for j in range(1, 8)]
            seq = [x for x in seq if x]
            for j, x in enumerate(seq):
                if x.mnemonic in ('beq', 'bne') and int(x.op_str.lstrip('#'), 0) <= i.address:
                    c = next((q for q in reversed(seq[:j])
                              if q.mnemonic in ('cmp', 'tst') and ', #' in q.op_str), None)
                    if c and not any(q.mnemonic.startswith('str') for q in seq[:j]):
                        v = int(c.op_str.split('#')[1], 0)
                        if c.mnemonic == 'cmp':
                            polls[i.address] = v if x.mnemonic == 'bne' else (0 if v else 1)
                        else:   # tst: bne loops while set -> return 0; beq loops while clear -> return mask
                            polls[i.address] = 0 if x.mnemonic == 'bne' else v
                    break
        return polls

    def _code_off(self, pc):
        if SRAM_BASE <= pc < SRAM_BASE + len(self.body):
            return pc - SRAM_BASE
        if DRAM_BASE <= pc < DRAM_BASE + len(self.body):
            return pc - DRAM_BASE
        return None

    def _modeled(self, addr):
        return (addr in (UART3, UART_LSR, DELAY_COUNTER, GPMC_STATUS, NAND_DATA,
                         GPMC_PREFETCH_STATUS)
                or addr in GPMC_BCH_RESULTS or I2C3 <= addr < I2C3 + 0x100
                or PREFETCH_FIFO <= addr < PREFETCH_FIFO + 0x100000)

    # --- hardware models -------------------------------------------------
    def _on_write(self, uc, access, addr, size, value, _):
        if addr == NAND_CMD:
            self.nand.command(value & 0xff)
        elif addr == NAND_ADDR:
            self.nand.address(value)
        elif self.eeprom and addr == I2C_DATA:
            self.eeprom.write_byte(value)
        elif self.eeprom and addr == I2C_CON and value & 0x1:      # STT
            self.eeprom.start(transmit=bool(value & 0x200))
        elif addr == UART3:
            self.console.append(value & 0xff)
            if self.verbose:
                sys.stdout.write(chr(value & 0xff))

    def _on_read(self, uc, access, addr, size, value, _):
        if addr == UART_LSR:
            uc.mem_write(addr, struct.pack('<I', 0x60)[:size])   # THR empty
            return
        if addr == GPMC_STATUS:
            uc.mem_write(addr, struct.pack('<I', 0x100)[:size])  # NAND ready
            return
        if addr == NAND_DATA:
            if self.nand.read_id_mode():
                v = self.nand.read(1)[0]                          # one ID byte per cycle
            else:
                v = int.from_bytes(self.nand.read(2), 'little')   # x16 word
            uc.mem_write(addr, (v & 0xffff).to_bytes(4, 'little')[:size])
            return
        if addr == GPMC_PREFETCH_STATUS:
            uc.mem_write(addr, struct.pack('<I', 0x40 << 24)[:size])
            return
        if addr in GPMC_BCH_RESULTS:
            uc.mem_write(addr, bytes(size))       # zero syndrome = no bit errors
            return
        if PREFETCH_FIFO <= addr < PREFETCH_FIFO + 0x100000:
            uc.mem_write(addr, self.nand.read(size))
            return
        if I2C3 <= addr < I2C3 + 0x100:
            if self.eeprom is None:
                return                                   # no EEPROM: reads as zeros
            if addr == I2C_STAT:
                uc.mem_write(addr, struct.pack('<I', 0x0114)[:size])   # ARDY|RRDY|XRDY
            elif addr == I2C_SYSS:
                uc.mem_write(addr, struct.pack('<I', 1)[:size])        # reset done
            elif addr == I2C_DATA:
                uc.mem_write(addr, struct.pack('<I', self.eeprom.read_byte())[:size])
            return
        if addr == DELAY_COUNTER:
            self.ticks = (self.ticks + 64) & 0xffffffff
            uc.mem_write(addr, struct.pack('<I', self.ticks)[:size])
            return
        off = self._code_off(uc.reg_read(UC_ARM_REG_PC))
        if off in self.polls and not self._modeled(addr) and addr < DRAM_BASE:
            uc.mem_write(addr, (self.polls[off] & ((1 << (8 * size)) - 1)).to_bytes(size, 'little'))

    def _on_unmapped(self, uc, access, addr, size, value, _):
        page = addr & ~0xfffff
        self.unmapped.append((hex(uc.reg_read(UC_ARM_REG_PC)), hex(addr)))
        uc.mem_map(page, 0x100000)
        return True

    def _on_code(self, uc, pc, size, _):
        if self._code_off(pc) in self.smc:
            uc.reg_write(UC_ARM_REG_PC, pc + 4)

    def _cpu_state(self, uc, pc):
        return {
            'pc': pc,
            'regs': [uc.reg_read(UC_ARM_REG_R0 + i) for i in range(4)],
            'cpsr': uc.reg_read(UC_ARM_REG_CPSR),
            'sctlr': uc.reg_read(UC_ARM_REG_CP_REG, (15, 0, 0, 1, 0, 0, 0)),
        }

    def _on_handoff(self, uc, pc, size, _):
        if self.handoff is None:
            self.handoff = self._cpu_state(uc, pc)
            if not self.follow:
                uc.emu_stop()
        elif IPL_LOAD_ADDR <= pc < IPL_LOAD_ADDR + 0x2A00000:
            # Code running inside the payload the IPL loaded = kernel entry.
            self.kernel_entry = self._cpu_state(uc, pc)
            uc.emu_stop()

    def _on_intr(self, uc, intno, _):
        pc = uc.reg_read(UC_ARM_REG_PC)
        raise RuntimeError(f'CPU exception {intno} at pc={pc:#x}')

    def run(self, max_insns):
        try:
            self.u.emu_start(SRAM_BASE, 0, count=max_insns)
        except UcError as e:
            print(f'\n[emu] UcError {e} at pc={self.u.reg_read(UC_ARM_REG_PC):#x}')
        return self.u.reg_read(UC_ARM_REG_PC)


IFS_BLOCKS = 336        # confirmed: nand_partition.txt IFS slot size
# Slot-valid flags (IPL 0x40201234): EEPROM bytes 0..7 are read; slot i is
# valid iff byte[i] == VALID_MAGIC[i] and byte[i + 4] == byte[i] (a mirror
# copy). Rejecting a slot zeroes both bytes.
VALID_MAGIC = bytes([0x33, 0x66, 0x99, 0xCC])
EE_CURRENT_IFS = 0xFD0  # IPL writes 1 << slot here before trying a slot


def default_eeprom(valid_slots=(0, 1, 2)):
    """8 KiB EEPROM image with the given slots marked valid."""
    e = bytearray(b'\xff' * 0x2000)
    for i in range(4):
        v = VALID_MAGIC[i] if i in valid_slots else 0
        e[i] = e[i + 4] = v
    return e


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('--ipl', required=True, help='stock ipl-hbas-dra526-hdisys-nand.bin')
    ap.add_argument('--ifs', help='IFS image to place in NAND')
    ap.add_argument('--slot', type=int, default=0, choices=(0, 1, 2))
    ap.add_argument('--no-eeprom', action='store_true')
    ap.add_argument('--max-insns', type=int, default=20_000_000)
    ap.add_argument('-v', '--verbose', action='store_true')
    a = ap.parse_args()
    nand = Nand()
    if a.ifs:
        nand.load_image(open(a.ifs, 'rb').read(), 4 + a.slot * IFS_BLOCKS)
    eeprom = None if a.no_eeprom else Eeprom(default_eeprom())
    emu = IplEmu(a.ipl, nand, eeprom, a.verbose)
    pc = emu.run(a.max_insns)
    print(f'\n[emu] stopped at pc={pc:#x}')
    h = emu.handoff
    if h:
        sc = h['sctlr']
        print(f"[emu] HANDOFF to {h['pc']:#010x}: r0-r3={[hex(r) for r in h['regs']]} "
              f"cpsr={h['cpsr']:#x} (mode {h['cpsr'] & 0x1f:#x}, IRQ {'off' if h['cpsr'] & 0x80 else 'on'}) "
              f"sctlr={sc:#x} (MMU {'on' if sc & 1 else 'off'}, D-cache {'on' if sc & 4 else 'off'}, "
              f"I-cache {'on' if sc & 0x1000 else 'off'})")
    else:
        print('[emu] no handoff: the IPL did not jump to an image')
    print('[emu] console:\n' + emu.console.decode('latin-1'))
    if eeprom:
        print('[emu] EEPROM read offsets:', [hex(o) for o in eeprom.log])
        print('[emu] EEPROM writes by IPL:', [(hex(o), hex(v)) for o, v in eeprom.writes])
    print('[emu] NAND log (first 12):', nand.log[:12])
    if emu.unmapped:
        print('[emu] first unmapped accesses:', emu.unmapped[:10])


if __name__ == '__main__':
    main()
