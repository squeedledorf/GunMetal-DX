#!/usr/bin/env python3
"""SPIR-V size / site counter for the embedded world shaders (#85 / #869 shadow core report).

Usage:
  python3 -I indra/vkclean/spvcount.py build-linux-x86_64/newview/vkshaders/vk_world_shaders_spv.h SYM [SYM ...]
  (the header is generated at build time, CMake target vk_shaders)
  python3 -I indra/vkclean/spvcount.py file.spv [file.spv ...]

Per shader: words / bytes, ray-query sites (OpRayQueryInitializeKHR 4473, Proceed 4477), image
samples (87-90), fetches (95), reads (98), functions (54) and loops (OpLoopMerge 246). The ray-query
initialize count is the number of inlined shadow / probe walks -- the compile-cost measure the
generic shadow core (clean_shadow_core.glsl) is judged by.
"""
import collections
import re
import struct
import sys


def words_from_header(txt, name):
    m = re.search(r'static const uint32_t ' + re.escape(name) + r'\[\] = \{(.*?)\};', txt, re.S)
    if not m:
        raise SystemExit(f"{name}: not found")
    return [int(x, 16) for x in re.findall(r'0x([0-9a-fA-F]+)u', m.group(1))]


def words_from_spv(path):
    data = open(path, 'rb').read()
    return list(struct.unpack(f"<{len(data) // 4}I", data))


def count(w):
    i = 5
    c = collections.Counter()
    while i < len(w):
        wc = w[i] >> 16
        op = w[i] & 0xffff
        c[op] += 1
        if wc == 0:
            break
        i += wc
    return c


def report(name, w):
    c = count(w)
    print(f"{name}: words {len(w)} bytes {len(w) * 4} RQInit {c[4473]} RQProceed {c[4477]} "
          f"ImageSample {c[87] + c[88] + c[89] + c[90]} ImgFetch {c[95]} ImgRead {c[98]} "
          f"Functions {c[54]} Loops {c[246]}")


def main():
    if len(sys.argv) < 2:
        raise SystemExit(__doc__)
    if sys.argv[1].endswith('.h'):
        txt = open(sys.argv[1]).read()
        for n in sys.argv[2:]:
            report(n, words_from_header(txt, n))
    else:
        for p in sys.argv[1:]:
            report(p, words_from_spv(p))


if __name__ == '__main__':
    main()
