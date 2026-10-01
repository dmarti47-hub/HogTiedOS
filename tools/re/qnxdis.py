#!/usr/bin/env python3
"""Disassemble a stripped QNX ARM ELF executable with readable annotations.

    qnxdis.py BINARY > out.dis

- Linear sweep, one word at a time, of the first PT_LOAD (code) segment, so
  data words in literal pools don't stop the disassembly.
- `bl` targets that are PLT stubs are annotated with the imported function
  name (resolved through DT_JMPREL relocations).
- movw/movt pairs and literal-pool words that point at a C string are
  annotated with the string.

Read-only analysis; needs capstone and pyelftools.
"""
import re
import struct
import sys

from capstone import Cs, CS_ARCH_ARM, CS_MODE_ARM
from elftools.elf.elffile import ELFFile


def load(path):
    data = open(path, 'rb').read()
    elf = ELFFile(open(path, 'rb'))
    loads = [s for s in elf.iter_segments() if s['p_type'] == 'PT_LOAD']
    return data, elf, loads


def v2o(loads, v):
    for s in loads:
        if s['p_vaddr'] <= v < s['p_vaddr'] + s['p_filesz']:
            return v - s['p_vaddr'] + s['p_offset']
    return None


def plt_names(data, elf, loads, lines):
    dyn = [s for s in elf.iter_segments() if s['p_type'] == 'PT_DYNAMIC'][0]
    tags = {t.entry.d_tag: t.entry.d_val for t in dyn.iter_tags()}
    if 'DT_JMPREL' not in tags:
        return {}
    symtab, strtab = v2o(loads, tags['DT_SYMTAB']), v2o(loads, tags['DT_STRTAB'])
    jmprel = v2o(loads, tags['DT_JMPREL'])
    got = {}
    for i in range(0, tags['DT_PLTRELSZ'], 8):
        off, info = struct.unpack_from('<II', data, jmprel + i)
        nm = struct.unpack_from('<I', data, symtab + 16 * (info >> 8))[0]
        got[off] = data[strtab + nm:data.index(b'\0', strtab + nm)].decode()
    plt = {}
    for n, (a, mn, op) in enumerate(lines[:-2]):
        # stub: add ip, pc, #0, #12 ; add ip, ip, #X ; ldr pc, [ip, #Y]!
        if mn == 'add' and op.startswith('ip, pc,'):
            try:
                x = int(lines[n + 1][2].split('#')[1], 0)
                y = int(lines[n + 2][2].split('#')[1].rstrip(']!'), 0)
            except (IndexError, ValueError):
                continue
            target = a + 8 + x + y
            if target in got:
                plt[a] = got[target]
    return plt


def cstring(data, loads, addr):
    o = v2o(loads, addr)
    if o is None:
        return None
    end = data.find(b'\0', o, o + 200)
    s = data[o:end] if end > o else b''
    if len(s) >= 3 and all(32 <= c < 127 or c in (9, 10) for c in s):
        return s.decode().replace('\n', '\\n')
    return None


def main():
    data, elf, loads = load(sys.argv[1])
    seg = loads[0]
    base, off, size = seg['p_vaddr'], seg['p_offset'], seg['p_filesz']
    md = Cs(CS_ARCH_ARM, CS_MODE_ARM)
    lines = []
    for o in range(0, size - 3, 4):
        a = base + o
        ins = list(md.disasm(data[off + o:off + o + 4], a))
        word = struct.unpack_from('<I', data, off + o)[0]
        lines.append((a, ins[0].mnemonic, ins[0].op_str) if ins else (a, '.word', f'{word:#010x}'))
    plt = plt_names(data, elf, loads, lines)
    movw = {}
    for n, (a, mn, op) in enumerate(lines):
        note = ''
        m = re.match(r'(r\d+|sb|sl|fp|ip|lr), #(0x[0-9a-f]+|\d+)$', op)
        if mn == 'movw' and m:
            movw[m.group(1)] = int(m.group(2), 0)
        elif mn == 'movt' and m and m.group(1) in movw:
            s = cstring(data, loads, int(m.group(2), 0) << 16 | movw[m.group(1)])
            if s:
                note = f'"{s}"'
        if mn == 'bl':
            t = int(op.lstrip('#'), 0)
            if t in plt:
                note = plt[t]
        if mn == '.word':
            s = cstring(data, loads, int(op, 16))
            if s:
                note = f'"{s}"'
        print(f'{a:08x}: {mn} {op}' + (f'   ; {note}' if note else ''))


if __name__ == '__main__':
    main()
