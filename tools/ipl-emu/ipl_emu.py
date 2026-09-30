#!/usr/bin/env python3
"""Run the stock Boom! Box IPL in Unicorn against simulated hardware.

Purpose: check offline whether the *real* stock IPL would accept a given
IFS image, without touching any hardware. The IPL binary comes from the
stock firmware (not in this repo; see --ipl).

Stage 1 (this file, in progress): boot the IPL from reset and capture its
UART3 console output, to find where hardware models are needed.
"""
import argparse
import struct
import sys

from unicorn import (Uc, UcError, UC_ARCH_ARM, UC_MODE_ARM, UC_HOOK_CODE,
                     UC_HOOK_MEM_READ, UC_HOOK_MEM_WRITE,
                     UC_HOOK_MEM_UNMAPPED, UC_HOOK_INTR)
from unicorn.arm_const import (UC_ARM_REG_PC, UC_ARM_REG_SP, UC_ARM_REG_CPSR,
                               UC_CPU_ARM_CORTEX_A8)
from capstone import Cs, CS_ARCH_ARM, CS_MODE_ARM

SRAM_BASE, SRAM_SIZE = 0x40200000, 0x10000
DRAM_BASE, DRAM_SIZE = 0x80000000, 0x10000000        # 256 MiB (both variants have >= this)
UART3 = 0x49020000                                    # debug console (confirmed)
UART_LSR = UART3 + 0x14
# Free-running counter the IPL's delay loop (0x40200E90) spins on; the loop
# masks it and handles wrap, so any monotonic tick works.
DELAY_COUNTER = 0x49030028


class IplEmu:
    def __init__(self, ipl_file, verbose=False):
        raw = open(ipl_file, 'rb').read()
        size, load = struct.unpack_from('<II', raw, 0)
        assert load == SRAM_BASE, hex(load)
        self.body = raw[8:8 + size]
        self.verbose = verbose
        self.console = bytearray()
        self.ticks = 0
        self.unmapped = []
        self.u = u = Uc(UC_ARCH_ARM, UC_MODE_ARM, cpu=UC_CPU_ARM_CORTEX_A8)
        u.mem_map(SRAM_BASE, SRAM_SIZE)
        u.mem_map(DRAM_BASE, DRAM_SIZE)
        u.mem_write(SRAM_BASE, self.body)
        # Peripheral windows as plain RAM; specific registers get models below.
        for base, sz in [(0x48000000, 0x400000), (0x49000000, 0x100000),
                         (0x6c000000, 0x3000000)]:
            u.mem_map(base, sz)
        u.hook_add(UC_HOOK_MEM_WRITE, self._on_write)
        u.hook_add(UC_HOOK_MEM_READ, self._on_read)
        u.hook_add(UC_HOOK_MEM_UNMAPPED, self._on_unmapped)
        u.hook_add(UC_HOOK_INTR, self._on_intr)
        # The boot ROM hands over with SP in on-chip SRAM.
        u.reg_write(UC_ARM_REG_SP, SRAM_BASE + SRAM_SIZE - 0x10)
        # `smc #0` calls go to the ROM's secure monitor (cache/errata
        # maintenance on this GP device). Treat them as no-ops.
        md = Cs(CS_ARCH_ARM, CS_MODE_ARM)
        self.insns = self._sweep(md)
        self.smc = {i.address for i in self.insns if i.mnemonic == 'smc'}
        u.hook_add(UC_HOOK_CODE, self._on_code)
        self.polls = self._find_polls()

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
        return addr in (UART3, UART_LSR, DELAY_COUNTER)

    # --- hardware models -------------------------------------------------
    def _on_write(self, uc, access, addr, size, value, _):
        if addr == UART3:
            self.console.append(value & 0xff)
            if self.verbose:
                sys.stdout.write(chr(value & 0xff))

    def _on_read(self, uc, access, addr, size, value, _):
        if addr == UART_LSR:
            uc.mem_write(addr, struct.pack('<I', 0x60)[:size])   # THR empty
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

    def _on_intr(self, uc, intno, _):
        pc = uc.reg_read(UC_ARM_REG_PC)
        raise RuntimeError(f'CPU exception {intno} at pc={pc:#x}')

    def run(self, max_insns):
        try:
            self.u.emu_start(SRAM_BASE, 0, count=max_insns)
        except UcError as e:
            print(f'\n[emu] UcError {e} at pc={self.u.reg_read(UC_ARM_REG_PC):#x}')
        return self.u.reg_read(UC_ARM_REG_PC)


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('--ipl', required=True, help='stock ipl-hbas-dra526-hdisys-nand.bin')
    ap.add_argument('--max-insns', type=int, default=20_000_000)
    ap.add_argument('-v', '--verbose', action='store_true')
    a = ap.parse_args()
    emu = IplEmu(a.ipl, a.verbose)
    pc = emu.run(a.max_insns)
    print(f'\n[emu] stopped at pc={pc:#x}')
    print('[emu] console:\n' + emu.console.decode('latin-1'))
    if emu.unmapped:
        print('[emu] first unmapped accesses:', emu.unmapped[:10])


if __name__ == '__main__':
    main()
