#!/usr/bin/env python3
"""Pack CorvaeOboro's cloud volumes into gmdx_cloud_shapes.bin for the volumetric clouds.

Source: https://github.com/CorvaeOboro/clouds_unreal (CC0). Its clouds are Unreal pseudo-volume
textures: an RGBA16F atlas of Z slices in an uncooked .uasset, stored as chunked zlib bulk data.
This reads the atlas straight out of the package (no Unreal needed), finds the slice grid,
crops each cloud to its bounds, pads it to a cube (aspect kept, resting on the cube's floor,
centred across) and averages it down to SIZE^3 bytes of density.

Usage: corvae_shapes.py <clouds_unreal checkout> <out.bin>
File: b"GMDXCS01", uint32 count, uint32 size, then count x size^3 uint8 (x fastest, then y, z).
"""
import io
import re
import struct
import sys
import zlib
from pathlib import Path

import numpy as np

SIZE = 64
# The volumes used (gmdx_cloud_common.glsl picks one per cloud). Left out: 03 and Wisp (their
# 8-bit atlases are not slice grids), storm (a flat vortex, not a cloud body).
NAMES = ["Cumulus_mediocris", "04", "05", "06", "08", "09", "bubble", "bubblemulti"]


def atlas(path):
    b = path.read_bytes()
    m = re.search(rb"Dimensions\x00.{4}(\d+)x(\d+)", b, re.S)
    w, h = int(m.group(1)), int(m.group(2))
    # The source bulk data: consecutive zlib chunks after the editor thumbnail (a PNG). It holds
    # the raw RGBA16F texels, or (8-bit sources) a PNG of them.
    i = b.find(b"IEND") + 8
    out = bytearray()
    while i < len(b) - 1 and len(out) < w * h * 8:
        if b[i] == 0x78 and b[i + 1] in (0x01, 0x5E, 0x9C, 0xDA):
            d = zlib.decompressobj()
            try:
                o = d.decompress(b[i:])
                if d.eof:
                    out += o
                    i = len(b) - len(d.unused_data)
                    continue
            except zlib.error:
                pass
        i += 1
    if out[:8] == b"\x89PNG\r\n\x1a\n":
        from PIL import Image
        img = np.asarray(Image.open(io.BytesIO(bytes(out))).convert("L"), np.float32) / 255.0
        assert img.shape == (h, w)
        return img
    assert len(out) >= w * h * 8, path
    return np.frombuffer(bytes(out[: w * h * 8]), np.float16).reshape(h, w, 4)[..., 0].astype(np.float32)


def volume(a):
    # Slices tile the atlas n x n; Unreal's pseudo-volumes are (near) cubes: n^2 slices of a / n.
    n = min((8, 16, 32), key=lambda n: abs(np.log((n * n) / (a.shape[0] / n))))
    t = a.shape[0] // n
    return a.reshape(n, t, n, t).transpose(0, 2, 1, 3).reshape(n * n, t, t)  # [z, y, x]


def pack(v):
    idx = np.argwhere(v > 0.02)
    lo, hi = idx.min(0), idx.max(0) + 1
    v = v[lo[0]:hi[0], lo[1]:hi[1], lo[2]:hi[2]]
    side = int(np.ceil(max(v.shape) / SIZE) * SIZE) + SIZE // 16 * (max(v.shape) // SIZE)
    side = int(np.ceil(side / SIZE) * SIZE)
    cube = np.zeros((side, side, side), np.float32)
    z, y, x = v.shape
    # One empty texel layer under the floor and around: trilinear reads never bleed between shapes.
    f = side // SIZE
    oy, ox = (side - y) // 2, (side - x) // 2
    cube[f:f + z, oy:oy + y, ox:ox + x] = v[: side - f]
    c = cube.reshape(SIZE, f, SIZE, f, SIZE, f).mean((1, 3, 5))
    c[-1], c[:, 0], c[:, -1], c[:, :, 0], c[:, :, -1] = 0, 0, 0, 0, 0
    c /= max(float(c.max()), 1e-6)
    return np.round(np.clip(c, 0, 1) * 255).astype(np.uint8)


def main():
    src, out = Path(sys.argv[1]), Path(sys.argv[2])
    tex = src / "CLOUDS_UNREAL5/Content/VFX_CLOUDS/TEXTURES"
    shapes = []
    for n in NAMES:
        v = volume(atlas(tex / f"VFX_CLOUD_{n}_VDB_TEX.uasset"))
        p = pack(v)
        assert p.shape == (SIZE, SIZE, SIZE) and p.max() == 255 and p[-1].max() == 0
        print(f"{n}: slices {v.shape}, filled {float((p > 8).mean()):.3f}")
        shapes.append(p)
    with open(out, "wb") as f:
        f.write(b"GMDXCS01" + struct.pack("<II", len(shapes), SIZE))
        for p in shapes:
            f.write(p.tobytes())


if __name__ == "__main__":
    main()
