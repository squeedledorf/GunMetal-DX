#!/usr/bin/env python3
"""Read a Slipstream snow dump (#783; RenderVKWeatherSnowDump) and report what built every wall.

The viewer writes logs/snowdump-<time>.bin when RenderVKWeatherSnowDump is set to 1: one FULL snow
mesh build (vkgfx/llcleanweathervk.cpp planDump / readDump; clean_weather_snowmesh.comp, the kD*
record words) around the camera's focus -- every texel's inputs (base, depth, cover, joins, the
depth relaxation's pulls, the apron field), every cell's branch, and the mesh (vertices and
triangles touching the window) as drawn BEFORE the build and AFTER it.

    python3 scripts/dev/snowdump.py ~/.slipstreamviewer/logs/snowdump-20261022-223000.bin
        [--floor Z] [--min-height 0.05] [--margin 10] [--out DIR] [--layer inner|ring|both] [--top 60]

Output (stdout + files next to the dump, or in --out):
  * WALLS: every emitted triangle steeper than the angle of repose + margin and taller than
    --min-height (sheet / apron triangles), and every VISIBLE skirt (a skirt no apron triangle
    covers), each with ONE branch code and the inputs that produced it (see BRANCHES below);
  * OUTLINE: every snowy texel beside a column it cannot join (<dump>.outline.csv);
  * COVER SINK / TRENCHES: where snowCoverSink pulls the depth, against which columns (their top,
    footprint), and how deep the snow is there against the global depth (the post-field craters);
  * PRE / POST: how the mesh drawn before the forced build differs from the build (#946 patches,
    stale interval builds);
  * PNG heatmaps (numpy + PIL): depth, base, field height, branch, walls; text without them.

scripts/dev/snowdump_resim.py re-runs the apron field / apron triangles offline from the same dump
under each #783 round's rules (r9 / r10 / r11; it reports which one reproduces the dump's own field).

--floor: the true floor beside / under the cover (world z, the same as the build floater's z).
Default: the focus point's z when the dump was centred on the camera's focus -- alt-click the
floor right beside the wall before arming the dump.
"""

import argparse
import csv
import json
import math
import os
import struct
import sys
from array import array
from collections import Counter, defaultdict

# ---- the record (clean_weather_snowmesh.comp kD*; keep in step) ----
F_VFLAGS, F_H, F_D, F_DIMG, F_RAWTOP, F_SUP, F_COVR, F_COVG = 0, 1, 2, 3, 4, 5, 6, 7
F_SMOOTH, F_S0, F_LIFT, F_BAND, F_X, F_Y, F_ZA, F_FA = 8, 9, 10, 11, 12, 13, 14, 15
F_GA, F_WG, F_NB, F_CYAN, F_TOEEX, F_SINK, F_REPOSE, F_DN = 16, 17, 18, 19, 20, 21, 22, 23  # (F_DN: round 10 on; 0 before)
F_CFLAGS, F_CEXTRA, F_CDMIN, F_CTRI, F_CZAP, F_CREJ = 24, 25, 26, 27, 28, 29
FLOAT_FIELDS = {F_H, F_D, F_DIMG, F_RAWTOP, F_SUP, F_COVR, F_COVG, F_SMOOTH, F_S0, F_LIFT, F_BAND, F_X, F_Y,
                F_ZA, F_FA, F_GA, F_TOEEX, F_SINK, F_REPOSE, F_DN, F_CDMIN, F_CZAP}
# vflags
V_BASE, V_SNOWY, V_EDGESHIFT, V_SPIKE, V_FIELD, V_WIDE, V_ATFLOOR = 1, 2, 4, 8, 0x10, 0x20, 0x40
V_MORPH1, V_MORPH2, V_CYAN, V_COVERNB, V_CONV, V_WRITTEN = 0x80, 0x100, 0x400, 0x800, 0x1000, 0x80000000
# kSnowN8 (clean_weather_snow_join.glsl)
N8 = [(1, 0), (-1, 0), (0, 1), (0, -1), (1, 1), (-1, -1), (-1, 1), (1, -1)]
SEAM, JOIN_SLOPE, APRON_TEXELS, COVER_TOE = 0.03, 1.0, 12, 0.7

BRANCHES = {
    'SHEET_STEEP': 'a plain sheet triangle spans a step: its corners are JOINED (slope allowance or the '
                   'lower snow reaches the higher base), so no skirt / apron is involved (case c: never enters the apron path)',
    'SHEET_STEEP_BARE': 'a sheet triangle drops to a bare / thin corner (marching squares) across a joined step (case c)',
    'APRON_REACH': 'an apron triangle steeper than the repose because the source is deeper than reach x tan(repose) '
                   '(g = depth / (n w) > tanR): the apron is attempted but cannot reach its floor at the repose (case a)',
    'APRON_GEOM': 'an apron triangle steeper than the repose although its drop rate is the repose (the slope arithmetic / '
                  'triangulation, case b)',
    'SKIRT_DROP': 'a visible skirt toward a LOWER base it cannot join (a drop: skirt by design, no apron -- case c)',
    'SKIRT_NOBASE': 'a visible skirt toward a column with no base (case c)',
    'SKIRT_FIELD_MISSING': 'a visible skirt toward a higher (cover) column where the closure / apron field never reached '
                           '(the cyan debug tint) -- the sub-reason names the failing first-step test (case a/c)',
    'SKIRT_DILATION_LOST': 'a visible skirt toward a higher column: the first step (a) passes from this texel, yet the field is '
                           'absent -- a field / window / convergence BUG (case a)',
    'SKIRT_APRON_REJECTED': 'the field reached the cover column but apronCell emitted no triangle over this edge (a corner '
                            'missing, a sheet corner whose base stands above the apron, an edge steeper than allowed)',
    'SKIRT_APRON_CELL_DROPPED': 'the apron cell across was never emitted (view cone, hand-over band, budget, stride)',
    'SKIRT_JOINED_OPEN': 'a visible skirt although the texels across are joined and their cell is emitted at the same stride',
    'SKIRT_SEAM': 'NOT A WALL: a skirt toward joined texels whose block has another stride (a T-junction seam skirt: it '
                  'hangs inside the snow under the neighbour\'s sheet) or was dropped by the view cone (off screen)',
    'SKIRT_OTHER': 'a visible skirt the script cannot attribute (see the inputs)',
    'STRIDE_PANEL': 'the owner cell has stride > 1 on a boundary (#946 edge LOD)',
}


def die(msg):
    sys.stderr.write('snowdump: ' + msg + '\n')
    sys.exit(1)


def u2f(u):
    return struct.unpack('<f', struct.pack('<I', u & 0xFFFFFFFF))[0]


def joined(ba, sa, bb, sb, dist):
    if abs(sa - sb) <= SEAM + JOIN_SLOPE * dist:
        return 'slope'
    if ba <= bb:
        return 'fill' if sa >= bb else None
    return 'fill' if sb >= ba else None


class Dump:
    def __init__(self, path):
        with open(path, 'rb') as fh:
            raw = fh.read()
        if raw[:8] != b'SNOWDUMP':
            die('not a snow dump: ' + path)
        ver, hl = struct.unpack_from('<II', raw, 8)
        self.meta = json.loads(raw[16:16 + hl].decode('utf-8'))
        base = 16 + hl
        self.sec = {}
        for s in self.meta['sections']:
            b = raw[base + s['offset']: base + s['offset'] + s['bytes']]
            a = array('I' if s['dtype'] == 'u4' else 'f')
            a.frombytes(b)
            if sys.byteorder != 'little':
                a.byteswap()
            self.sec[s['name']] = a
        m = self.meta
        self.W = m['window']
        self.RW = m['record_words']
        self.HW = m['head_words']
        self.rec = self.sec['records']
        self.layers = {0: m['inner'], 1: m['ring']}
        for L in (0, 1):
            g = self.layers[L]
            g['w'] = (g['extent'] / g['res']) if g['res'] else 0.0
        self.tanR = m['tan_repose']
        self.thr = m['thr']
        self.nvI = m['inner']['nv']
        self.nvR = m['ring']['nv']
        self.terrain = self.sec.get('terrain')
        self.verts = {}
        self.tris = {}
        for k in ('pre', 'post'):
            v = self.sec[k + '_verts']
            d = {}
            for i in range(0, len(v), 5):
                d[v[i]] = (u2f(v[i + 1]), u2f(v[i + 2]), u2f(v[i + 3]), v[i + 4])
            self.verts[k] = d
            t = self.sec[k + '_tris']
            self.tris[k] = [(t[i], t[i + 1], t[i + 2], t[i + 3]) for i in range(0, len(t), 4)]

    # ---- records ----
    def slot(self, L, tx, ty):
        o = self.layers[L]['origin']
        i, j = tx - o[0], ty - o[1]
        if i < 0 or j < 0 or i >= self.W or j >= self.W:
            return None
        return (L * self.W + j) * self.W + i

    def word(self, s, f):
        return self.rec[self.HW + s * self.RW + f]

    def val(self, s, f):
        w = self.word(s, f)
        return u2f(w) if f in FLOAT_FIELDS else w

    def texel(self, L, tx, ty):
        """A dict of the texel's record (None outside the window / never written)."""
        s = self.slot(L, tx, ty)
        if s is None:
            return None
        fl = self.word(s, F_VFLAGS)
        r = {'L': L, 't': (tx, ty), 'slot': s, 'written': bool(fl & V_WRITTEN), 'vflags': fl}
        for f in range(self.RW):
            r[f] = self.val(s, f)
        r['base'] = bool(fl & V_BASE)
        r['S'] = r[F_H] + r[F_D] if r['base'] else None
        w = self.word(s, F_WG)
        r['n'], r['it'], r['conv'] = w & 0xFF, (w >> 8) & 0xFF, bool(w & 0x10000)
        nb = self.word(s, F_NB)
        r['jm'], r['cm'], r['nbm'], r['fm'] = nb & 0xFF, (nb >> 8) & 0xFF, (nb >> 16) & 0xFF, (nb >> 24) & 0xFF
        cy = self.word(s, F_CYAN)
        r['cyan4'] = [(cy >> (4 * i)) & 15 for i in range(4)]
        r['covm'] = (cy >> 16) & 0xFF
        c = self.word(s, F_CFLAGS)
        r['cwritten'] = bool(c & 0x80)
        r['st'] = c & 7
        r['onstride'], r['cone'], r['bandcull'] = bool(c & 8), bool(c & 0x10), bool(c & 0x20)
        r['mask'], r['diag'], r['amask'], r['adiag'] = (c >> 8) & 3, (c >> 10) & 1, (c >> 11) & 3, (c >> 13) & 1
        r['apc'], r['open'], r['skirts'] = (c >> 14) & 15, (c >> 18) & 15, (c >> 22) & 63
        r['over'] = bool(c & 0x10000000)
        r['why'] = (c >> 29) & 7
        ce = self.word(s, F_CEXTRA)
        r['lodst'], r['edgelod'], r['ntri'] = ce & 7, (ce >> 3) & 3, (ce >> 24) & 0xFF
        r['rej'] = self.word(s, F_CREJ)
        r['field'] = bool(fl & V_FIELD)
        return r

    def texels(self, L):
        o = self.layers[L]['origin']
        for j in range(self.W):
            for i in range(self.W):
                r = self.texel(L, o[0] + i, o[1] + j)
                if r and r['written']:
                    yield r

    # ---- vertices ----
    def vdecode(self, vi):
        """(layer, slot, tx, ty) of a vertex index."""
        if vi < 3 * self.nvI:
            res = self.layers[0]['res']
            return 0, vi // self.nvI, (vi % self.nvI) % res, (vi % self.nvI) // res
        o = vi - 3 * self.nvI
        res = self.layers[1]['res']
        return 1, o // self.nvR, (o % self.nvR) % res, (o % self.nvR) // res

    def terrain_at(self, L, tx, ty):
        if self.terrain is None:
            return None
        o = self.layers[L]['origin']
        i, j = tx - o[0], ty - o[1]
        if i < 0 or j < 0 or i >= self.W or j >= self.W:
            return None
        v = self.terrain[(L * self.W + j) * self.W + i]
        return None if math.isnan(v) else v


def tri_slope(p0, p1, p2):
    ux, uy, uz = p1[0] - p0[0], p1[1] - p0[1], p1[2] - p0[2]
    vx, vy, vz = p2[0] - p0[0], p2[1] - p0[1], p2[2] - p0[2]
    nx, ny, nz = uy * vz - uz * vy, uz * vx - ux * vz, ux * vy - uy * vx
    ln = math.sqrt(nx * nx + ny * ny + nz * nz)
    if ln < 1e-12:
        return None
    return math.degrees(math.acos(min(1.0, abs(nz) / ln)))


def floor_source(D, r, floor_user):
    """Where the base the apron ends on (the source's own base) comes from."""
    h = r[F_H]
    terr = D.terrain_at(r['L'], *r['t'])
    water = D.meta.get('water')
    tags = []
    if terr is not None and abs(h - terr) <= 0.05:
        tags.append('terrain')
    elif terr is not None:
        tags.append('prim/map top (%.2f m above the terrain)' % (h - terr))
    if water is not None and h <= water + 0.02:
        tags.append('AT/UNDER WATER (%.2f)' % water)
    if abs(r[F_RAWTOP] - h) > 0.05 and r['L'] == 0:
        tags.append('filtered base differs from the raw column top by %.2f' % (r[F_RAWTOP] - h))
    if floor_user is not None:
        tags.append('%.2f vs the given floor' % (h - floor_user))
    return '; '.join(tags) or '?'


def cyan_reason(nib):
    """The first step (a) re-evaluated from the sheet texel toward one 4-neighbour."""
    parts = []
    if nib & 1:
        parts.append('higher column, not joined')
    if nib & 2:
        parts.append('NO FIELD there')
    parts.append('step (a) PASSES' if nib & 4 else 'step (a) fails (cover not above the candidate / joined / thin)')
    if nib & 8:
        parts.append('a LOWER base, not joined (drop)')
    return ', '.join(parts)


def apron_rej_text(rej):
    t = []
    if not rej & 0x100:
        t.append('no apron corner in the cell')
    missing = [i for i in range(4) if not (rej >> i) & 1]
    if missing:
        t.append('corners without a vertex %s' % missing)
    up = [i for i in range(4) if (rej >> (4 + i)) & 1]
    if up:
        t.append('sheet corners dropped: base above the apron %s' % up)
    steep = [i for i in range(4) if (rej >> (13 + i)) & 1]
    if steep:
        t.append('triangles dropped for a steep edge (diag*2+k) %s' % steep)
    t.append('masks d0=%d d1=%d' % ((rej >> 9) & 3, (rej >> 11) & 3))
    return '; '.join(t)


def analyse(D, args):
    out_rows = []
    floor_user = args.floor
    if floor_user is None and D.meta.get('focus_is_focus') and D.meta.get('focus'):
        floor_user = D.meta['focus'][2]
    tanR = D.tanR
    repose = math.degrees(math.atan(tanR))
    lim = repose + args.margin
    layers = {'inner': [0], 'ring': [1], 'both': [0, 1]}[args.layer]
    V = D.verts['post']

    # triangle ordinal -> owner cell (the records' first triangle + count)
    owner = {}
    for L in layers:
        for r in D.texels(L):
            if r['cwritten'] and r[F_CTRI] != 0xFFFFFFFF and r['ntri']:
                for k in range(r['ntri']):
                    owner[r[F_CTRI] + k] = r

    # apron triangles' top-vertex pairs (a skirt under one is hidden)
    apron_edges = set()
    tri_list = []
    for (ordn, a, b, c) in D.tris['post']:
        ds = [D.vdecode(x) for x in (a, b, c)]
        if ds[0][0] not in layers:
            continue
        slots = [d[1] for d in ds]
        kind = 'skirt' if 1 in slots else ('apron' if 2 in slots else 'sheet')
        tri_list.append((ordn, (a, b, c), ds, kind))
        if kind == 'apron':
            tops = [x for x, d in zip((a, b, c), ds) if d[1] == 0]
            for i in range(len(tops)):
                for j in range(i + 1, len(tops)):
                    apron_edges.add(frozenset((tops[i], tops[j])))

    walls = []
    seen_skirt = set()
    for (ordn, idx, ds, kind) in tri_list:
        if any(x not in V for x in idx):
            continue
        P = [V[x] for x in idx]
        own = owner.get(ordn)
        L = ds[0][0]
        w = D.layers[L]['w']
        if kind == 'skirt':
            tops = sorted(set(x for x, d in zip(idx, ds) if d[1] == 0))
            texs = sorted(set((d[2], d[3]) for d in ds))
            if len(texs) != 2:
                continue
            key = (L, tuple(texs))
            if key in seen_skirt:
                continue
            seen_skirt.add(key)
            ra, rb = D.texel(L, *texs[0]), D.texel(L, *texs[1])
            if not ra or not rb:
                continue
            hgt = max(ra[F_S0] - min(ra[F_H], ra[F_S0]), rb[F_S0] - min(rb[F_H], rb[F_S0]))
            res = D.layers[L]['res']
            vi_a = (0 if L == 0 else 3 * D.nvI) + texs[0][1] * res + texs[0][0]
            vi_b = (0 if L == 0 else 3 * D.nvI) + texs[1][1] * res + texs[1][0]
            hidden = frozenset((vi_a, vi_b)) in apron_edges
            if hidden or hgt < args.min_height:
                continue
            walls.append(classify_skirt(D, L, texs, ra, rb, own, hgt, floor_user))
        else:
            sl = tri_slope(P[0], P[1], P[2])
            if sl is None:
                continue
            span = max(p[2] for p in P) - min(p[2] for p in P)
            if sl <= lim or span < args.min_height:
                continue
            rs = [D.texel(L, d[2], d[3]) for d in ds]
            if any(r is None for r in rs):
                continue
            walls.append(classify_steep(D, L, kind, ds, rs, P, sl, span, own, floor_user))

    # ---- report ----
    print('== snow dump: window %d^2, inner res %d (texel %.3f m), ring %s; repose %.1f deg (tan %.3f), threshold %.3f m'
          % (D.W, D.layers[0]['res'], D.layers[0]['w'], 'on' if D.layers[1]['on'] else 'off', repose, tanR, D.thr))
    print('   global depth %.3f m, water %.2f, edge LOD %d, toe reach option %.2f m, smooth %s, small VRAM %s'
          % (D.meta['global_depth'], D.meta.get('water') or 0, D.meta['edge_lod'], D.meta['toe_reach_opt'],
             D.meta['smooth'], D.meta['small_vram']))
    print('   focus %s (%s); floor used for the comparisons: %s'
          % (D.meta['focus'], 'camera focus' if D.meta.get('focus_is_focus') else 'camera position',
             '%.2f' % floor_user if floor_user is not None else 'none (pass --floor)'))
    w0 = D.layers[0]['w']
    n0 = max(2, min(APRON_TEXELS, math.ceil(max(D.meta['toe_reach_opt'], min(COVER_TOE, APRON_TEXELS * w0)) / w0 - 0.001)))
    print('   inner apron reach n = %d texels = %.2f m: drops at most %.2f m at the repose'
          % (n0, n0 * w0, n0 * w0 * tanR))
    seams = [wl for wl in walls if wl['branch'] == 'SKIRT_SEAM']
    walls = [wl for wl in walls if wl['branch'] != 'SKIRT_SEAM']
    cnt = Counter(wl['branch'] for wl in walls)
    print('\n== WALLS: %d (steeper than %.0f deg and taller than %.2f m, or a visible skirt); + %d seam skirts (SKIRT_SEAM, '
          'hidden: not counted)' % (len(walls), lim, args.min_height, len(seams)))
    for b, n in cnt.most_common():
        print('  %5d  %-26s %s' % (n, b, BRANCHES.get(b, '')))
    sub = Counter((wl['branch'], wl.get('sub', '')) for wl in walls)
    print('\n  by sub-reason:')
    for (b, s), n in sub.most_common(40):
        print('  %5d  %-26s %s' % (n, b, s))
    print('\n  the first %d (sorted by height):' % args.top)
    for wl in sorted(walls, key=lambda x: -x['height'])[:args.top]:
        print('  %-26s L%d %-12s h %.2f m  %s' % (wl['branch'], wl['L'], str(wl['texels'][0]), wl['height'], wl['detail']))

    base = args.prefix
    with open(base + '.walls.csv', 'w', newline='') as fh:
        cw = csv.writer(fh)
        cw.writerow(['branch', 'sub', 'layer', 'tx', 'ty', 'height_m', 'detail'])
        for wl in walls:
            cw.writerow([wl['branch'], wl.get('sub', ''), wl['L'], wl['texels'][0][0], wl['texels'][0][1],
                         '%.3f' % wl['height'], wl['detail']])

    outline(D, layers, floor_user, base)
    sink_report(D, layers)
    prepost(D, layers)
    heatmaps(D, walls, base)
    print('\nfiles: %s.walls.csv, %s.outline.csv%s' % (base, base, ', %s.*.png' % base if HAVE_PIL else ''))


def classify_skirt(D, L, texs, ra, rb, own, hgt, floor_user):
    wl = {'L': L, 'texels': texs, 'height': hgt, 'kind': 'skirt'}
    st = own['st'] if own else 1
    # the owner triangle's third corner -> the outward side
    across = []
    if own:
        ox, oy = own['t']
        corners = [(ox + st * (i & 1), oy + st * (i >> 1)) for i in range(4)]
        a, b = texs
        if a[0] != b[0] and a[1] != b[1]:
            # (a diagonal skirt: the corner the owner triangle does not hold -- the one not joined to the edge)
            rest = [c for c in corners if c not in (a, b)]
            wd = D.layers[L]['w'] * st
            def jn(c):
                rc = D.texel(L, *c)
                return bool(rc and rc['base'] and ra['base'] and rb['base']
                            and joined(ra[F_H], ra['S'], rc[F_H], rc[F_H] + rc[F_D], wd)
                            and joined(rb[F_H], rb['S'], rc[F_H], rc[F_H] + rc[F_D], wd))
            hi = ra if (ra['S'] or -1e9) >= (rb['S'] or -1e9) else rb
            across = [(hi, c) for c in ([c for c in rest if not jn(c)] or rest)]
        else:
            inner_c = [c for c in corners if c not in (a, b)]
            if a[0] == b[0]:
                nx = 1 if all(c[0] < a[0] for c in inner_c) else -1
                across = [(ra, (a[0] + nx * st, a[1])), (rb, (b[0] + nx * st, b[1]))]
            else:
                ny = 1 if all(c[1] < a[1] for c in inner_c) else -1
                across = [(ra, (a[0], a[1] + ny * st)), (rb, (b[0], b[1] + ny * st))]
    src = ra if (ra['S'] or -1e9) >= (rb['S'] or -1e9) else rb
    w = D.layers[L]['w']
    reasons = []
    branch = 'SKIRT_OTHER'
    detail = []
    for e, q in across:
        rq = D.texel(L, *q)
        src = e if e['base'] else src
        if rq is None or not rq['written']:
            reasons.append(('SKIRT_OTHER', 'across outside the window'))
            continue
        if not rq['base']:
            reasons.append(('SKIRT_NOBASE', 'no base across'))
            continue
        jr = joined(src[F_H], src['S'], rq[F_H], rq[F_H] + rq[F_D], w * st)
        if rq[F_H] > src['S'] + SEAM and not jr:
            # a cover / wall: the closure / apron should continue the sheet
            d = (q[0] - src['t'][0], q[1] - src['t'][1])
            nib = None
            for i in range(4):
                if N8[i] == (int(math.copysign(1, d[0])) if d[0] else 0, int(math.copysign(1, d[1])) if d[1] else 0):
                    nib = src['cyan4'][i]
            if not rq['field']:
                if nib is not None and nib & 4:
                    reasons.append(('SKIRT_DILATION_LOST', cyan_reason(nib)))
                else:
                    reasons.append(('SKIRT_FIELD_MISSING', cyan_reason(nib) if nib is not None else 'not a 4-neighbour'))
            else:
                # the field reached: the cell across emitted no apron triangle over this edge
                ax, ay = min(c[0] for c in [ra['t'], rb['t'], q]), min(c[1] for c in [ra['t'], rb['t'], q])
                rc = D.texel(L, ax, ay)
                if rc and rc['cwritten']:
                    if not rc['onstride'] or rc['cone'] or rc['bandcull'] or rc['over']:
                        reasons.append(('SKIRT_APRON_CELL_DROPPED', 'across cell %s: on stride %s cone %s band %s over budget %s st %d'
                                        % (rc['t'], rc['onstride'], rc['cone'], rc['bandcull'], rc['over'], rc['st'])))
                    else:
                        reasons.append(('SKIRT_APRON_REJECTED', 'across cell %s amask %d: %s'
                                        % (rc['t'], rc['amask'], apron_rej_text(rc['rej']))))
                else:
                    reasons.append(('SKIRT_APRON_REJECTED', 'across cell not recorded'))
            detail.append('across %s base %.2f (top %.2f, raw %.2f) %s'
                          % (q, rq[F_H], rq[F_H] + rq[F_D], rq[F_RAWTOP],
                             ('field za %.2f fa %.2f g %+.2f' % (rq[F_ZA], rq[F_FA], rq[F_GA])) if rq['field'] else 'NO field'))
        elif rq[F_H] < src[F_H] and not jr:
            reasons.append(('SKIRT_DROP', 'across base %.2f (its snow top %.2f) is %.2f m below the sheet base %.2f'
                            % (rq[F_H], rq[F_H] + rq[F_D], src[F_H] - rq[F_H], src[F_H])))
        elif jr:
            # the across cell: its block's stride / the view cone (the anchor of the cell holding q)
            sq = rq['st'] or 1
            anc = D.texel(L, min(q[0], e['t'][0]) & ~(sq - 1), min(q[1], e['t'][1]) & ~(sq - 1))
            if sq != st or (anc and (anc['cone'] or anc['bandcull'])):
                reasons.append(('SKIRT_SEAM', 'joined (%s) across %s: stride %d vs %d%s'
                                % (jr, q, sq, st, ', cone-culled' if anc and anc['cone'] else '')))
            else:
                reasons.append(('SKIRT_JOINED_OPEN', 'joined (%s) across: %s' % (jr, q)))
        else:
            reasons.append(('SKIRT_OTHER', 'across base %.2f between the sheet base %.2f and its top %.2f, not joined'
                            % (rq[F_H], src[F_H], src['S'])))
    order = ['SKIRT_DILATION_LOST', 'SKIRT_FIELD_MISSING', 'SKIRT_APRON_REJECTED', 'SKIRT_APRON_CELL_DROPPED', 'SKIRT_DROP',
             'SKIRT_NOBASE', 'SKIRT_JOINED_OPEN', 'SKIRT_OTHER', 'SKIRT_SEAM']
    if reasons:
        branch, sub = sorted(reasons, key=lambda r: order.index(r[0]))[0]
    else:
        sub = 'no owner cell record'
    if st > 1:
        sub += ' [STRIDE %d on a boundary]' % st
    reach = src['n'] * w
    detail.insert(0, 'sheet S %.2f = base %.2f + depth %.2f (img %.2f); floor src: %s; required drop to the given floor %s; '
                     'reach %d tx = %.2f m -> achievable %.2f m at the repose; sink %.3f toeEx %+.2f; cyan bit %s'
                  % (src['S'], src[F_H], src[F_D], src[F_DIMG], floor_source(D, src, floor_user),
                     ('%.2f' % (src['S'] - floor_user)) if floor_user is not None else '?',
                     src['n'], reach, reach * D.tanR, src[F_SINK], src[F_TOEEX], bool(src['vflags'] & V_CYAN)))
    wl.update(branch=branch, sub=sub, detail=' | '.join(detail))
    return wl


def classify_steep(D, L, kind, ds, rs, P, sl, span, own, floor_user):
    w = D.layers[L]['w']
    texs = [(d[2], d[3]) for d in ds]
    wl = {'L': L, 'texels': texs, 'height': span, 'kind': kind}
    det = ['slope %.0f deg over %.2f m' % (sl, span)]
    if kind == 'apron':
        ap = [r for r, d in zip(rs, ds) if d[1] == 2]
        g = max(abs(r[F_GA]) for r in ap)
        n = max(r['n'] for r in ap)
        fa = min(r[F_FA] for r in ap)
        srcs = [r for r, d in zip(rs, ds) if d[1] == 0]
        sdepth = max((r[F_D] for r in srcs), default=0.0)
        if g > D.tanR * 1.02:
            branch, sub = 'APRON_REACH', 'g %.2f > tanR %.2f: source depth %.2f > reach %d x %.3f x tanR = %.2f' % (
                g, D.tanR, sdepth, n, w, n * w * D.tanR)
        else:
            branch, sub = 'APRON_GEOM', 'g %.2f = the repose; triangle corners slots %s' % (g, [d[1] for d in ds])
        det.append('apron floor fa %.2f (%s)' % (fa, floor_source(D, srcs[0], floor_user) if srcs else '?'))
        if floor_user is not None:
            det.append('fa %.2f vs the given floor (%+.2f)' % (fa, fa - floor_user))
    else:
        rules = []
        bare = False
        for i in range(3):
            a, b = rs[i], rs[(i + 1) % 3]
            dist = w * math.hypot(a['t'][0] - b['t'][0], a['t'][1] - b['t'][1])
            jr = joined(a[F_H], a[F_H] + a[F_D], b[F_H], b[F_H] + b[F_D], dist)
            rules.append(jr or 'CUT?')
            bare = bare or a[F_D] <= D.thr
        branch = 'SHEET_STEEP_BARE' if bare else 'SHEET_STEEP'
        sub = 'joins %s' % '/'.join(rules)
        det.append('corners (base, depth): ' + ', '.join('(%.2f, %.2f)' % (r[F_H], r[F_D]) for r in rs))
        det.append('floor src: ' + floor_source(D, rs[0], floor_user))
    if own and own['st'] > 1:
        sub += ' [STRIDE %d]' % own['st']
    wl.update(branch=branch, sub=sub, detail=' | '.join(det))
    return wl


def outline(D, layers, floor_user, base):
    """Every snowy texel beside a column it cannot join (the cover / drop outline)."""
    rows = []
    for L in layers:
        w = D.layers[L]['w']
        for r in D.texels(L):
            if not r['base'] or r[F_D] <= D.thr or not (r['cm'] & 0x0F):
                continue
            for i in range(4):
                if not (r['cm'] >> i) & 1:
                    continue
                q = (r['t'][0] + N8[i][0], r['t'][1] + N8[i][1])
                rq = D.texel(L, *q)
                kind = 'cover' if (r['covm'] >> i) & 1 else 'drop' if (rq and rq[F_H] < r[F_H]) else 'cut'
                terr = D.terrain_at(L, *r['t'])
                rows.append([L, r['t'][0], r['t'][1], i, kind, '%.3f' % r[F_H], '%.3f' % r[F_D], '%.3f' % r[F_DIMG],
                             '%.3f' % r['S'], '%.3f' % (rq[F_H] if rq else float('nan')),
                             '%.3f' % (rq[F_D] if rq else float('nan')),
                             int(bool(rq and rq['field'])), '%.3f' % (rq[F_ZA] if rq else float('nan')),
                             '%.3f' % (rq[F_FA] if rq else float('nan')), '%.3f' % (rq[F_GA] if rq else float('nan')),
                             r['n'], '%.3f' % (r['n'] * w), int(bool(r['vflags'] & V_CYAN)), r['cyan4'][i],
                             '%.4f' % r[F_SINK], '%.4f' % r[F_REPOSE], '%.3f' % r[F_TOEEX],
                             '%.3f' % terr if terr is not None else '', '%.3f' % r[F_RAWTOP],
                             '%.3f' % (r['S'] - floor_user) if floor_user is not None else ''])
    with open(base + '.outline.csv', 'w', newline='') as fh:
        cw = csv.writer(fh)
        cw.writerow(['layer', 'tx', 'ty', 'nb', 'kind', 'base', 'depth', 'depth_img', 'snow_top', 'nb_base', 'nb_depth',
                     'nb_field', 'nb_za', 'nb_fa', 'nb_g', 'reach_tx', 'reach_m', 'cyan', 'step_a_nibble', 'sink',
                     'repose_pull', 'toe_excess', 'terrain', 'raw_top', 'drop_to_floor'])
        cw.writerows(rows)
    kinds = Counter(r[4] for r in rows)
    cy = sum(1 for r in rows if r[17])
    print('\n== OUTLINE: %d snowy texel / neighbour pairs not joined: %s; cyan (field missing beside a cover): %d'
          % (len(rows), dict(kinds), cy))


def sink_report(D, layers):
    """snowCoverSink: where it pulls, against which columns, and the craters it leaves."""
    gd = D.meta['global_depth']
    for L in layers:
        w = D.layers[L]['w']
        recs = {r['t']: r for r in D.texels(L)}
        pulled = [r for r in recs.values() if r['base'] and r[F_SINK] < -1e-5]
        # the cover columns: a texel some snowy neighbour sees as a cover
        cov = set()
        for r in recs.values():
            if r['base'] and r['covm']:
                for i in range(8):
                    if (r['covm'] >> i) & 1:
                        cov.add((r['t'][0] + N8[i][0], r['t'][1] + N8[i][1]))
        # connected footprints (8-connected cover texels)
        comps, seen = [], set()
        for c in cov:
            if c in seen:
                continue
            stack, comp = [c], []
            seen.add(c)
            while stack:
                p = stack.pop()
                comp.append(p)
                for dx, dy in N8:
                    q = (p[0] + dx, p[1] + dy)
                    if q in cov and q not in seen:
                        seen.add(q)
                        stack.append(q)
            comps.append(comp)
        print('\n== COVER SINK (layer %d): %d texels pulled by snowCoverSink (share 1: %.3f m total pull); %d cover columns in %d footprints'
              % (L, len(pulled), sum(r[F_SINK] for r in pulled), len(cov), len(comps)))
        rad = max(1, int(round(1.0 / w))) if w else 1
        rows = []
        for comp in comps:
            tops = [recs[p][F_H] for p in comp if p in recs and recs[p]['base']]
            if not tops:
                continue
            cx = sum(p[0] for p in comp) / len(comp)
            cy = sum(p[1] for p in comp) / len(comp)
            # the snow within ~1 m (near) vs 1.5 .. 3 m (far) on bases below the footprint's top
            near, far = [], []
            ext = len(comp) ** 0.5
            # (the ground the footprint stands in: the bases of the snowy texels that see it as a cover)
            gb = sorted(r[F_H] for r in recs.values() if r['base'] and r['covm'] and any(
                (r['t'][0] + N8[i][0], r['t'][1] + N8[i][1]) in comp for i in range(8) if (r['covm'] >> i) & 1))
            if not gb:
                continue
            g0 = gb[len(gb) // 2]
            for p, r in recs.items():
                if p in cov or not r['base'] or abs(r[F_H] - g0) > 0.5:
                    continue
                dd = max(abs(p[0] - cx), abs(p[1] - cy)) - ext
                if dd <= rad:
                    near.append(r[F_D])
                elif 1.5 * rad <= dd <= 3 * rad:
                    far.append(r[F_D])
            if not near:
                continue
            far.sort()
            fmed = far[len(far) // 2] if far else float('nan')
            rows.append((len(comp), len(comp) * w * w, max(tops), min(near), sum(near) / len(near), fmed, (cx, cy)))
        rows.sort(key=lambda x: x[3])
        print('   footprint texels (its outline) / m2 / top z / snow depth within ~1 m: min, mean / median 1.5-3 m out '
              '(global depth %.2f; the ground within 0.5 m of the base around it) / centre texel' % gd)
        for n, a, top, dmin, dmean, fmed, c in rows[:25]:
            # a CRATER: the snow near it far below the snow around it (which itself holds snow): a bare
            # steep roof / dome around a post is not one
            crater = ' CRATER' if (fmed == fmed and fmed > 0.3 * gd and dmean < 0.6 * fmed) else ''
            print('   %5d  %6.2f m2  top %7.2f  near min %.2f mean %.2f  far %.2f  at (%.0f, %.0f)%s'
                  % (n, a, top, dmin, dmean, fmed, c[0], c[1], crater))


def prepost(D, layers):
    pre, post = D.verts['pre'], D.verts['post']
    moved = 0
    gone = 0
    big = []
    for vi, p in post.items():
        L, slot, tx, ty = D.vdecode(vi)
        if L not in layers:
            continue
        q = pre.get(vi)
        if q is None:
            continue
        if (q[0], q[1], q[2], q[3]) == (0.0, 0.0, 0.0, 0) and (p[0], p[1], p[2]) != (0.0, 0.0, 0.0):
            gone += 1
            continue
        dz = p[2] - q[2]
        if abs(dz) > 0.02:
            moved += 1
            big.append((abs(dz), slot, tx, ty, q[2], p[2]))
    pt = set(tuple(t[1:]) for t in D.tris['pre'])
    qt = set(tuple(t[1:]) for t in D.tris['post'])
    print('\n== PRE / POST (the mesh drawn before the forced build -- %d recordings old, interval %d, patches %s -- vs the build)'
          % (D.meta['mesh_age_before'], D.meta['mesh_interval'], D.meta['mesh_patch']))
    print('   vertices moved > 2 cm: %d, empty before: %d; triangles only before: %d, only after: %d'
          % (moved, gone, len(pt - qt), len(qt - pt)))
    for b in sorted(big, reverse=True)[:10]:
        print('   slot %d texel (%d, %d): z %.3f -> %.3f' % (b[1], b[2], b[3], b[4], b[5]))


try:
    import numpy as np
    from PIL import Image
    HAVE_PIL = True
except Exception:  # (text only)
    HAVE_PIL = False

BRANCH_COLOURS = {0: (0, 0, 0), 1: (90, 90, 90), 2: (200, 220, 255), 3: (255, 160, 0), 4: (255, 0, 255), 5: (0, 255, 255),
                  6: (160, 0, 160), 7: (90, 0, 90), 8: (120, 80, 40), 9: (255, 255, 0)}
BRANCH_NAMES = {0: 'no base', 1: 'thin', 2: 'sheet', 3: 'sheet edge (drop)', 4: 'sheet beside a cover, field ok',
                5: 'sheet beside a cover, NO field (cyan)', 6: 'blocked: apron / closure vertex', 7: 'blocked: field at its floor',
                8: 'base without snow, no field', 9: 'sheet joined across a step > 0.2 m'}


def texel_class(D, r):
    if not r['base']:
        return 0
    f = r['vflags']
    if f & V_FIELD:
        return 7 if f & V_ATFLOOR else 6
    if r[F_D] <= D.thr:
        return 8 if r[F_D] <= 1e-4 else 1
    if f & V_CYAN:
        return 5
    if r['covm'] & 0x0F:
        return 4
    if any(((r['cyan4'][i] >> 3) & 1) for i in range(4)):
        return 3
    return 2


def heatmaps(D, walls, base):
    lines = []
    if not HAVE_PIL:
        print('\n(numpy / PIL missing: no PNGs; the branch map as text, inner layer, 1 char per 2 texels)')
        o = D.layers[0]['origin']
        for j in range(D.W - 1, -1, -2):
            row = ''
            for i in range(0, D.W, 2):
                r = D.texel(0, o[0] + i, o[1] + j)
                row += '.' if not r or not r['written'] else '0123456789'[texel_class(D, r)]
            lines.append(row)
        print('\n'.join(lines))
        return
    for L in (0, 1):
        if L == 1 and not D.layers[1]['on']:
            continue
        o = D.layers[L]['origin']
        W = D.W
        dep = np.full((W, W), np.nan, np.float32)
        bas = np.full((W, W), np.nan, np.float32)
        fld = np.full((W, W), np.nan, np.float32)
        snk = np.full((W, W), np.nan, np.float32)
        br = np.zeros((W, W, 3), np.uint8)
        for j in range(W):
            for i in range(W):
                r = D.texel(L, o[0] + i, o[1] + j)
                if not r or not r['written']:
                    continue
                if r['base']:
                    dep[j, i] = r[F_D]
                    bas[j, i] = r[F_H]
                    snk[j, i] = r[F_SINK]
                if r['field']:
                    fld[j, i] = r[F_ZA] - r[F_FA]
                br[j, i] = BRANCH_COLOURS[texel_class(D, r)]
        for wl in walls:
            if wl['L'] != L:
                continue
            for t in wl['texels']:
                i, j = t[0] - o[0], t[1] - o[1]
                if 0 <= i < W and 0 <= j < W:
                    br[j, i] = (255, 0, 0)

        def save(a, name, lo=None, hi=None):
            v = a[np.isfinite(a)]
            if v.size == 0:
                return
            lo = float(np.min(v)) if lo is None else lo
            hi = float(np.max(v)) if hi is None else hi
            n = np.nan_to_num(np.clip((a - lo) / max(hi - lo, 1e-6), 0, 1), nan=0.0)
            img = np.zeros((W, W, 3), np.uint8)
            img[..., 0] = (n * 255).astype(np.uint8)
            img[..., 1] = (np.sqrt(n) * 255).astype(np.uint8)
            img[..., 2] = ((1 - n) * 200).astype(np.uint8)
            img[~np.isfinite(a)] = 0
            Image.fromarray(img[::-1]).resize((W * 4, W * 4), Image.NEAREST).save('%s.L%d.%s.png' % (base, L, name))
            print('   %s.L%d.%s.png: %.3f .. %.3f' % (base, L, name, lo, hi))
        print('\n== PNG (y up; black: none)')
        save(dep, 'depth', 0.0)
        save(bas, 'base')
        save(fld, 'field')
        save(snk, 'sink', None, 0.0)
        Image.fromarray(br[::-1]).resize((W * 4, W * 4), Image.NEAREST).save('%s.L%d.branch.png' % (base, L))
        print('   %s.L%d.branch.png: red = a wall texel; ' % (base, L)
              + ', '.join('%s %s' % (BRANCH_COLOURS[k], v) for k, v in BRANCH_NAMES.items()))


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument('dump')
    ap.add_argument('--floor', type=float, default=None, help='the true floor z beside / under the cover (default: the focus z)')
    ap.add_argument('--min-height', type=float, default=0.05, help='walls lower than this (m) are ignored')
    ap.add_argument('--margin', type=float, default=10.0, help='degrees over the repose that count as a wall')
    ap.add_argument('--layer', choices=['inner', 'ring', 'both'], default='inner')
    ap.add_argument('--top', type=int, default=60)
    ap.add_argument('--out', default=None, help='directory for the CSV / PNG files (default: next to the dump)')
    args = ap.parse_args()
    D = Dump(args.dump)
    stem = os.path.splitext(os.path.basename(args.dump))[0]
    d = args.out or os.path.dirname(os.path.abspath(args.dump))
    os.makedirs(d, exist_ok=True)
    args.prefix = os.path.join(d, stem)
    analyse(D, args)


if __name__ == '__main__':
    main()
