#!/usr/bin/env python3
"""Content-hash every asset and texture in a Slipstream cache and report duplicates:
the same bytes stored under different UUIDs (common in SL: re-uploads, copies).

usage: cache_dedupe_stats.py [CACHE_DIR] [--hashes OUT.tsv] [--bench N]

  CACHE_DIR   default ~/.slipstreamviewer/cache
  --hashes    write "kind<TAB>uuid<TAB>bytes<TAB>blake2b-256<TAB>complete" for every entry
  --bench N   decode N sample textures with opj_decompress to estimate decode cost
              (default 24; 0 = skip)

Textures: texture.entries (EntriesInfo 44 B + Entry 28 B each) indexes texture.cache
(600 B header slot per entry) and texturecache/<c>/<uuid>.texture (the rest). The
stored bytes are header + body; an entry holding a partial image (fewer bytes than
the image's total size) only matches other entries cut at the same length, so
duplicates are counted per complete image and partial ones are reported separately.
Assets: cache/sl_cache_<uuid>_<n>.asset files, hashed whole, classified by header.
"""
import argparse, collections, hashlib, os, struct, subprocess, sys, tempfile, time

ENTRY_SIZE = 600
INFO = struct.Struct('<fI32sI')
ENTRY = struct.Struct('<16siiI')


def uuid_str(b):
    h = b.hex()
    return f'{h[0:8]}-{h[8:12]}-{h[12:16]}-{h[16:20]}-{h[20:32]}'


def asset_kind(h):
    if h.startswith(b'OggS'): return 'sound'
    if h[:4] == b'\x01\x00\x00\x00': return 'mesh' if h[12:13] == b'{' else 'animation'
    if h.startswith(b'<? LLSD/Binary ?>'): return 'llsd (material/settings)'
    if h.startswith(b'LLWearable'): return 'wearable'
    if h.startswith(b'Linden text version'): return 'notecard'
    if h[:2] == b'2\n': return 'gesture'
    if h[:4] == b'\xffO\xffQ': return 'j2c'
    return 'other'


def j2c_dims(data):
    # SIZ marker right after SOC: FF4F FF51 Lsiz(2) Rsiz(2) Xsiz(4) Ysiz(4) XOsiz(4) YOsiz(4)
    if len(data) < 24 or data[:4] != b'\xffO\xffQ':
        return 0, 0
    xs, ys, xo, yo = struct.unpack('>IIII', data[8:24])
    return max(0, xs - xo), max(0, ys - yo)


def human(n):
    for u in ('B', 'KB', 'MB', 'GB', 'TB'):
        if n < 1024 or u == 'TB':
            return f'{n:.1f} {u}' if u != 'B' else f'{n} B'
        n /= 1024


def scan_textures(cache, out):
    tdir = os.path.join(cache, 'texturecache')
    with open(os.path.join(tdir, 'texture.entries'), 'rb') as f:
        ver, addr, enc, n = INFO.unpack(f.read(INFO.size))
        raw = f.read(n * ENTRY.size)
    hdr = open(os.path.join(tdir, 'texture.cache'), 'rb')
    recs = []
    for i in range(n):
        idb, img, body, _t = ENTRY.unpack_from(raw, i * ENTRY.size)
        if idb == b'\0' * 16 or img <= 0:
            continue
        uid = uuid_str(idb)
        hdr.seek(i * ENTRY_SIZE)
        head = hdr.read(min(ENTRY_SIZE, img))
        data = head
        if body > 0:
            p = os.path.join(tdir, uid[0], uid + '.texture')
            try:
                with open(p, 'rb') as bf:
                    data = head + bf.read()
            except OSError:
                pass
        complete = len(data) >= img
        w, h = j2c_dims(data)
        d = hashlib.blake2b(data, digest_size=32).hexdigest()
        recs.append((uid, len(data), d, complete, w, h))
        if out:
            out.write(f'texture\t{uid}\t{len(data)}\t{d}\t{int(complete)}\n')
    return ver, recs


def scan_assets(cache, out):
    adir = os.path.join(cache, 'cache')
    recs = []
    for e in os.scandir(adir):
        if not (e.is_file() and e.name.startswith('sl_cache_')):
            continue
        uid = e.name[9:45]
        try:
            with open(e.path, 'rb') as f:
                data = f.read()
        except OSError:
            continue
        d = hashlib.blake2b(data, digest_size=32).hexdigest()
        recs.append((uid, len(data), d, asset_kind(data[:24])))
        if out:
            out.write(f'asset\t{uid}\t{len(data)}\t{d}\t1\n')
    return recs


def dup_stats(recs, key=lambda r: r[2]):
    groups = collections.defaultdict(list)
    for r in recs:
        groups[key(r)].append(r)
    dups = {k: v for k, v in groups.items() if len(v) > 1}
    extra_copies = sum(len(v) - 1 for v in dups.values())
    wasted = sum((len(v) - 1) * v[0][1] for v in dups.values())
    return groups, dups, extra_copies, wasted


def bench_decode(cache, recs, n):
    """ms per megapixel for a full-resolution opj_decompress of n sample textures."""
    tdir = os.path.join(cache, 'texturecache')
    sample = [r for r in recs if r[3] and r[4] * r[5] >= 256 * 256][:: max(1, len(recs) // max(n, 1))][:n]
    if not sample:
        return None
    tot_mp = tot_s = 0.0
    with tempfile.TemporaryDirectory() as td:
        for uid, size, _d, _c, w, h in sample:
            src = os.path.join(td, uid + '.j2k')
            # rebuild the stored bytes (header slot + body) via the entries index again is
            # costly; re-read through the same path as scan_textures for just this one.
            data = _read_texture(cache, uid)
            if not data:
                continue
            with open(src, 'wb') as f:
                f.write(data)
            dst = os.path.join(td, uid + '.tga')
            t0 = time.perf_counter()
            r = subprocess.run(['opj_decompress', '-i', src, '-o', dst], capture_output=True)
            dt = time.perf_counter() - t0
            if r.returncode == 0:
                tot_s += dt
                tot_mp += w * h / 1e6
    return (tot_s * 1000.0 / tot_mp) if tot_mp else None


_INDEX = None


def _read_texture(cache, uid):
    global _INDEX
    tdir = os.path.join(cache, 'texturecache')
    if _INDEX is None:
        _INDEX = {}
        with open(os.path.join(tdir, 'texture.entries'), 'rb') as f:
            _v, _a, _e, n = INFO.unpack(f.read(INFO.size))
            raw = f.read(n * ENTRY.size)
        for i in range(n):
            idb, img, body, _t = ENTRY.unpack_from(raw, i * ENTRY.size)
            if img > 0:
                _INDEX[uuid_str(idb)] = (i, img, body)
    if uid not in _INDEX:
        return None
    i, img, body = _INDEX[uid]
    with open(os.path.join(tdir, 'texture.cache'), 'rb') as h:
        h.seek(i * ENTRY_SIZE)
        data = h.read(min(ENTRY_SIZE, img))
    if body > 0:
        try:
            with open(os.path.join(tdir, uid[0], uid + '.texture'), 'rb') as bf:
                data += bf.read()
        except OSError:
            pass
    return data


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('cache', nargs='?', default=os.path.expanduser('~/.slipstreamviewer/cache'))
    ap.add_argument('--hashes')
    ap.add_argument('--bench', type=int, default=24)
    a = ap.parse_args()
    out = open(a.hashes, 'w') if a.hashes else None
    t0 = time.time()
    ver, tex = scan_textures(a.cache, out)
    assets = scan_assets(a.cache, out)
    if out:
        out.close()
    print(f'scanned in {time.time() - t0:.0f} s (texture cache version {ver:.2f})\n')

    comp = [r for r in tex if r[3]]
    part = [r for r in tex if not r[3]]
    _g, dups, extra, wasted = dup_stats(comp)
    tex_bytes = sum(r[1] for r in tex)
    print('TEXTURES')
    print(f'  entries {len(tex)} ({human(tex_bytes)}), complete {len(comp)}, partial {len(part)}')
    print(f'  complete images with duplicates: {len(dups)} distinct images stored {len(dups) + extra} times')
    print(f'  redundant copies: {extra} ({100 * extra / max(len(comp), 1):.1f}% of complete entries)')
    print(f'  wasted space: {human(wasted)} ({100 * wasted / max(tex_bytes, 1):.1f}% of texture cache)')
    _gp, dupp, extrap, wastedp = dup_stats(part)
    print(f'  partial entries identical byte-for-byte to another: {extrap} copies, {human(wastedp)}')
    big = sorted(dups.values(), key=lambda v: -(len(v) - 1) * v[0][1])[:5]
    for v in big:
        print(f'    e.g. {len(v)} copies x {human(v[0][1])} ({v[0][4]}x{v[0][5]}) e.g. {v[0][0]}')

    # wasted decode: every redundant copy is decoded separately (different UUID = different
    # LLViewerFetchedTexture). Full-resolution upper bound; the viewer decodes at the needed
    # discard, so typical cost is lower.
    wasted_mp = sum((len(v) - 1) * v[0][4] * v[0][5] / 1e6 for v in dups.values())
    print(f'  redundant megapixels (full res): {wasted_mp:,.0f} MP')
    if a.bench:
        ms = bench_decode(a.cache, comp, a.bench)
        if ms:
            cpu_s = wasted_mp * ms / 1000.0
            print(f'  opj_decompress full-res: {ms:.1f} ms/MP (single-threaded tool, {a.bench} samples)')
            print(f'  wasted decode for the redundant copies: ~{cpu_s / 60:.1f} CPU-minutes at full res '
                  f'(~{cpu_s / 60 / 4:.1f} at discard 1, ~{cpu_s / 60 / 16:.1f} at discard 2)')

    print('\nASSETS')
    asset_bytes = sum(r[1] for r in assets)
    _ga, dupa, extraa, wasteda = dup_stats(assets)
    print(f'  files {len(assets)} ({human(asset_bytes)}), redundant copies {extraa}, '
          f'wasted {human(wasteda)} ({100 * wasteda / max(asset_bytes, 1):.1f}%)')
    by = collections.defaultdict(lambda: [0, 0, 0, 0])   # files, bytes, extra copies, wasted
    for r in assets:
        by[r[3]][0] += 1
        by[r[3]][1] += r[1]
    for v in dupa.values():
        k = v[0][3]
        by[k][2] += len(v) - 1
        by[k][3] += (len(v) - 1) * v[0][1]
    for k, (nf, nb, ne, nw) in sorted(by.items(), key=lambda kv: -kv[1][1]):
        print(f'    {k:26s} {nf:7d} files {human(nb):>9s}  dup copies {ne:6d}  wasted {human(nw):>9s}')

    total = tex_bytes + asset_bytes
    print(f'\nTOTAL wasted {human(wasted + wastedp + wasteda)} of {human(total)} '
          f'({100 * (wasted + wastedp + wasteda) / max(total, 1):.1f}%)')


if __name__ == '__main__':
    sys.exit(main())
