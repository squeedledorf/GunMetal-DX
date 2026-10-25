#!/usr/bin/env python3
"""Rewrite the x86-64 ISA level an ELF executable declares it needs.

The linker ORs GNU_PROPERTY_X86_ISA_1_NEEDED across every input object. On a
distro that builds glibc for x86-64-v4 (e.g. CachyOS), the startup object
Scrt1.o carries a v4 note, so every executable we link claims to need AVX-512
even though our code is compiled for -march=x86-64-v2 -- and glibc's loader on
other distros refuses to start it on e.g. a Zen 3 CPU ("CPU ISA level is lower
than required"). This patches the property value in place (same size).

usage: fix_isa_note.py <elf> [mask]   mask: 1 baseline, 3 +v2 (default), 7 +v3, 15 +v4
"""
import struct
import sys

GNU_PROPERTY_X86_ISA_1_NEEDED = 0xC0008002
NT_GNU_PROPERTY_TYPE_0 = 5


def patch(path, mask):
    with open(path, "r+b") as f:
        data = bytearray(f.read())
        if data[:4] != b"\x7fELF" or data[4] != 2 or data[5] != 1:
            return 0  # not a little-endian ELF64: nothing to do
        shoff, = struct.unpack_from("<Q", data, 0x28)
        shentsize, shnum, shstrndx = struct.unpack_from("<HHH", data, 0x3A)
        if shoff == 0 or shnum == 0:
            return 0
        str_off, = struct.unpack_from("<Q", data, shoff + shstrndx * shentsize + 0x18)
        patched = 0
        for i in range(shnum):
            sh = shoff + i * shentsize
            name_off, sh_type = struct.unpack_from("<II", data, sh)
            off, size = struct.unpack_from("<QQ", data, sh + 0x18)
            end = data.index(b"\0", str_off + name_off)
            if data[str_off + name_off:end] != b".note.gnu.property":
                continue
            align, = struct.unpack_from("<Q", data, sh + 0x30)
            align = max(int(align), 4)
            up = lambda v: (v + align - 1) & ~(align - 1)
            p = off
            while p + 12 <= off + size:
                namesz, descsz, ntype = struct.unpack_from("<III", data, p)
                desc = off + up(p - off + 12 + namesz)
                if ntype == NT_GNU_PROPERTY_TYPE_0:
                    q = desc
                    while q + 8 <= desc + descsz:
                        pr_type, pr_datasz = struct.unpack_from("<II", data, q)
                        if pr_type == GNU_PROPERTY_X86_ISA_1_NEEDED and pr_datasz >= 4:
                            old, = struct.unpack_from("<I", data, q + 8)
                            if old != mask:
                                struct.pack_into("<I", data, q + 8, mask)
                                patched += 1
                                print(f"fix_isa_note: {path}: ISA_1_NEEDED 0x{old:x} -> 0x{mask:x}")
                        q += 8 + ((pr_datasz + 7) & ~7)
                p = off + up(desc - off + descsz)
        if patched:
            f.seek(0)
            f.write(data)
        return patched


if __name__ == "__main__":
    if len(sys.argv) < 2:
        sys.exit(__doc__)
    patch(sys.argv[1], int(sys.argv[2], 0) if len(sys.argv) > 2 else 3)
