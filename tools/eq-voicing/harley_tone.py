"""Run stock audioCtrlSvc's own tone-filter design in Unicorn.

fixed_tone_load_eq (0x1288a8) loads a profile's tag-0x61 records, and
fixed_tone_update_filters (0x128764) computes the 7 tone-slot biquads for
given control values (docs/findings/AUDIO.md sec. 7.3). Running Harley's code
instead of re-deriving its fixed-point formulas means the result is exactly
what the stock radio sends to the DSP. Read-only analysis of the firmware
the user supplies; nothing from it is copied into the output except the
derived curves.
"""
import struct

from elftools.elf.elffile import ELFFile
from unicorn import Uc, UC_ARCH_ARM, UC_MODE_ARM, UC_HOOK_CODE
from unicorn.arm_const import (UC_ARM_REG_R0, UC_ARM_REG_R1, UC_ARM_REG_R2,
                               UC_ARM_REG_R3, UC_ARM_REG_SP, UC_ARM_REG_LR,
                               UC_ARM_REG_PC)

# audioCtrlSvc sha256 a199b880c5662f13...: function and PLT addresses
LOAD_EQ, UPDATE_FILTERS = 0x1288a8, 0x128764
PLT = {0x104f38: 'memcpy', 0x1054b4: 'memset', 0x105178: 'uidiv'}
HEAP, STACK, STOP = 0x20000000, 0x30000000, 0x40000000
OBJ, FILT, COEF, PROF, RECS = HEAP, HEAP + 0x1000, HEAP + 0x2000, HEAP + 0x3000, HEAP + 0x4000
SLOTS = 7


class ToneDesigner:
    def __init__(self, audioctrlsvc_path):
        data = open(audioctrlsvc_path, 'rb').read()
        elf = ELFFile(open(audioctrlsvc_path, 'rb'))
        self.uc = uc = Uc(UC_ARCH_ARM, UC_MODE_ARM)
        mapped = set()
        for s in elf.iter_segments():
            if s['p_type'] != 'PT_LOAD':
                continue
            v, o, sz = s['p_vaddr'], s['p_offset'], s['p_filesz']
            base, end = v & ~0xfff, (v + s['p_memsz'] + 0xfff) & ~0xfff
            for page in range(base, end, 0x1000):
                if page not in mapped:
                    uc.mem_map(page, 0x1000)
                    mapped.add(page)
            uc.mem_write(v, data[o:o + sz])
        uc.mem_map(HEAP, 0x100000)
        uc.mem_map(STACK - 0x10000, 0x20000)
        uc.mem_map(STOP, 0x1000)
        uc.hook_add(UC_HOOK_CODE, self._plt, begin=min(PLT), end=max(PLT))

    @staticmethod
    def _plt(uc, addr, size, _):
        f = PLT.get(addr)
        if not f:
            return
        r0, r1, r2 = (uc.reg_read(r) for r in (UC_ARM_REG_R0, UC_ARM_REG_R1, UC_ARM_REG_R2))
        if f == 'memcpy':
            uc.mem_write(r0, bytes(uc.mem_read(r1, r2)))
        elif f == 'memset':
            uc.mem_write(r0, bytes([r1 & 0xff]) * r2)
        else:
            uc.reg_write(UC_ARM_REG_R0, r0 // r1)
        uc.reg_write(UC_ARM_REG_PC, uc.reg_read(UC_ARM_REG_LR))

    def _call(self, fn, *args):
        uc = self.uc
        for r, a in zip((UC_ARM_REG_R0, UC_ARM_REG_R1, UC_ARM_REG_R2, UC_ARM_REG_R3), args):
            uc.reg_write(r, a & 0xffffffff)
        uc.reg_write(UC_ARM_REG_SP, STACK)
        uc.reg_write(UC_ARM_REG_LR, STOP)
        uc.emu_start(fn, STOP, count=5_000_000)
        return uc.reg_read(UC_ARM_REG_R0)

    def design(self, records, bass=8, treble=8, volume_step=5, speed=0):
        """Biquads for the 7 tone slots, standard signs:
        {slot: (b0, b1, b2, a1, a2)}. Knob step 8 is flat for bass/treble."""
        uc = self.uc
        uc.mem_write(HEAP, bytes(0x8000))
        uc.mem_write(OBJ, struct.pack('<H', SLOTS))          # capacity
        uc.mem_write(OBJ + 4, struct.pack('<I', FILT))
        uc.mem_write(OBJ + 0x10, struct.pack('<I', COEF))
        prof = bytearray(0x80)
        struct.pack_into('<H', prof, 0x34, len(records))
        for i, r in enumerate(records):
            struct.pack_into('<H', prof, 0x36 + 2 * i, len(r))
            struct.pack_into('<I', prof, 0x44 + 4 * i, RECS + 0x40 * i)
            uc.mem_write(RECS + 0x40 * i, r)
        uc.mem_write(PROF, bytes(prof))
        if self._call(LOAD_EQ, OBJ, PROF) & 0xffff:
            raise ValueError('fixed_tone_load_eq rejected the records')
        # controls: +8 bass (1), +9 (2, unused), +0xa treble (4),
        # +0xb volume step - 1 (8: loudness), +0xc speed (16)
        uc.mem_write(OBJ + 8, bytes([bass, 8, treble, max(volume_step - 1, 0), speed]))
        self._call(UPDATE_FILTERS, OBJ, 1)
        raw = uc.mem_read(COEF, SLOTS * 20)
        out = {}
        for s in range(SLOTS):
            b0, b1, b2, A1, A2 = (x / 2**24 for x in struct.unpack_from('<5i', raw, 20 * s))
            out[s] = (b0, b1, b2, -A1, -A2)                  # DSP stores A negated
        return out
