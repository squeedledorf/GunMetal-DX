#!/usr/bin/env python3
"""#783: re-run the snow mesh's apron FIELD and APRON CELLS offline from a snow dump
(RenderVKWeatherSnowDump; snowdump.py) under each round's rules, and compare them:

  r9   the highest candidate wins; an apron edge up to 32 dz / dx (before round 10)
  r10  a source deeper by 0.1 m wins unless the other floor stands >= 6 m over it; an apron edge
       only where the surface is continuous at the field's own drop rate (round 10)
  r11  the LOWEST floor wins (floors more than kAprFloorSep apart; else the higher candidate); the
       round-10 continuity (round 11: clean_weather_snowmesh.comp aprBetter)
  r12  r11 + no depth on a texel inside a filter-opened thin feature at a drop (round 12:
       clean_weather_snow.comp kOpenedSkin) -- always replayed, from the dump's own depths
  r13  r12 + no depth on a texel standing more than kCanopyLift over the outer ring's surface there
       (round 13: kCanopyLift) -- always replayed

First checks which rule reproduces the dump's own field (the build that wrote it), then per rule:
the apron triangles steeper than the repose + 10 degrees and taller than 5 cm, and the OUTLINE EDGES
left without an apron (a snowy sheet texel beside a column over its top -- a cover or a wall -- whose
edge no apron triangle continues: its skirt shows) and the DROP SKIRTS (a snowy texel beside a
lower base it cannot join), split by level (the sheet's base under --floor-max,
default 20 m: the ground / floor; above: roofs). --cells x,y ... replays apronCell on those cells and
prints every corner and test.

    python3 scripts/dev/snowdump_resim.py <snowdump.bin> [--floor-max Z] [--cells 270,278 214,273]
"""
import argparse
import math
import os
import sys
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import snowdump as sd

ap_ = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
ap_.add_argument('dump')
ap_.add_argument('--floor-max', type=float, default=20.0)
ap_.add_argument('--cells', nargs='*', default=[])
ap_.add_argument('--rules', default='r9,r10,r11')
args = ap_.parse_args()
D = sd.Dump(args.dump)
L = 0
W = D.W
o = D.layers[0]['origin']
w = D.layers[0]['w']
tanR = D.tanR
thr = D.thr
SEAM, JS = 0.03, 1.0
CLEAR, CLOSE = 0.25, 2
N = 12
COVER_ABOVE = 6.0     # r10: clean_weather_snow_join.glsl kSnowCoverAbove
DEEPER = 0.1          # r10: kAprDeeper
FLOOR_SEP = 0.15      # r11: kAprFloorSep
SLACK = 0.02          # r10+: kAprSheetSlack
N8 = sd.N8
H = {}; Dd = {}; S0 = {}; ZA = {}; FA = {}; REC = {}
for j in range(W):
    for i in range(W):
        t = (o[0] + i, o[1] + j)
        r = D.texel(0, *t)
        if not r or not r['written']:
            continue
        REC[t] = r
        if r['base']:
            H[t] = r[sd.F_H]; Dd[t] = r[sd.F_D]; S0[t] = r[sd.F_S0]
        if r['field']:
            ZA[t] = r[sd.F_ZA]; FA[t] = r[sd.F_FA]


def joined(ba, sa, bb, sb, dist):
    if abs(sa - sb) <= SEAM + JS * dist:
        return True
    return sa >= bb if ba <= bb else sb >= ba


def gof(dn):
    return max(tanR, dn / (N * w))


def better(rule, cur, cand, dn, fl):
    """(cand, dn, floor fl) over cur = (z, floor, dn)."""
    if cur is None:
        return True
    if rule == 'r9':
        return cand > cur[0]
    if rule == 'r10':
        if dn > cur[2] + DEEPER and cur[1] - cand < COVER_ABOVE:
            return True
        if cur[2] > dn + DEEPER and fl - cur[0] < COVER_ABOVE:
            return False
        return cand > cur[0]
    # r11: the lowest floor (the snow under the cover; a sheet beside a higher column of its own --
    # a step, a wall -- keeps its skirt: snow lies flat against a wall)
    if abs(fl - cur[1]) > FLOOR_SEP:
        return fl < cur[1]
    return cand > cur[0]


def dilate(rule):
    F = {}   # t -> (z, floor, dn)
    for it in range(N):
        far = it >= CLOSE
        G = {}
        for t, hb in H.items():
            cur = F.get(t)
            Sc = hb + Dd[t]
            for (dx, dy) in N8:
                q = (t[0] + dx, t[1] + dy)
                dist = w * (1.41421356 if dx and dy else 1.0)
                if it == 0 and q in H and Dd[q] > thr:
                    hn, dn = H[q], Dd[q]
                    Sn = hn + dn
                    cand = Sn - gof(dn) * dist
                    if cand <= hn + 0.005:
                        cand = hn
                    if better(rule, cur, cand, dn, hn) and hb > cand + SEAM and not joined(hn, Sn, hb, Sc, dist):
                        cur = (cand, hn, dn)
                fq = F.get(q)
                if fq is None or fq[0] <= fq[1] + 0.005:
                    continue
                cand = fq[0] - gof(fq[2]) * dist
                if cand <= fq[1] + 0.005:
                    cand = fq[1]
                if better(rule, cur, cand, fq[2], fq[1]) and hb > cand + (CLEAR if far else SEAM):
                    cur = (cand, fq[1], fq[2])
            if cur is not None:
                G[t] = cur
        if G == F:
            break
        F = G
    return F


KTRI = [(0, 1, 3), (0, 3, 2), (0, 1, 2), (1, 3, 2)]


def apron_cell(F, rule, x, y, verbose=False):
    """clean_weather_snowmesh.comp apronCell at stride 1 -> [(corner ids, slope, height, corners)]."""
    r = REC.get((x, y))
    cs = [(x, y), (x + 1, y), (x, y + 1), (x + 1, y + 1)]
    ok, ap, z, g = [], [], [], []
    for c in cs:
        if c in F:
            ok.append(True); ap.append(True); z.append(F[c][0]); g.append(gof(F[c][2]))
        elif c in S0:
            ok.append(True); ap.append(False); z.append(S0[c]); g.append(0.0)
        else:
            ok.append(False); ap.append(False); z.append(0.0); g.append(0.0)
    if verbose:
        for i, c in enumerate(cs):
            fld = F.get(c)
            print('      corner %d %s: %s z %.3f base %s depth %s%s' % (
                i, c, 'APRON' if ap[i] else ('sheet' if ok[i] else 'none'), z[i],
                '%.3f' % H[c] if c in H else '-', '%.3f' % Dd[c] if c in Dd else '-',
                (' field floor %.3f src depth %.3f g %.2f' % (fld[1], fld[2], g[i])) if fld else ''))
    out = []
    if not any(a and k for a, k in zip(ap, ok)):
        if verbose:
            print('      -> no apron corner')
        return out
    zap = min(zz for zz, a in zip(z, ap) if a)
    for i in range(4):
        if ok[i] and not ap[i] and H[cs[i]] > zap + SEAM:
            ok[i] = False
            if verbose:
                print('      corner %d dropped: its base %.3f stands over the lowest apron %.3f' % (i, H[cs[i]], zap))
    m = [0, 0]
    for dg in range(2):
        for k in range(2):
            c = KTRI[dg * 2 + k]
            if not all(ok[i] for i in c) or not any(ap[i] for i in c):
                continue
            good = True
            for jj in range(3):
                a, b = c[jj], c[(jj + 1) % 3]
                ln = w * (1.41421356 if (a ^ b) == 3 else 1.0)
                if ap[a] or ap[b]:
                    if rule == 'r9':
                        lim = SEAM + 32.0 * ln
                    else:
                        lim = SEAM + (0.0 if (ap[a] and ap[b]) else SLACK) + max(g[a], g[b]) * ln
                else:
                    lim = SEAM + JS * ln
                if abs(z[a] - z[b]) > lim:
                    good = False
                    if verbose:
                        print('      tri %d (diag %d) edge %d-%d: |dz| %.3f > %.3f' % (dg * 2 + k, dg, a, b, abs(z[a] - z[b]), lim))
            if good:
                m[dg] |= 1 << k
    if r and r['mask']:
        dg = r['diag']
    elif bin(m[0]).count('1') != bin(m[1]).count('1'):
        dg = 0 if bin(m[0]).count('1') > bin(m[1]).count('1') else 1
    else:
        dg = 0 if abs(z[0] - z[3]) <= abs(z[1] - z[2]) else 1
    for k in range(2):
        if m[dg] >> k & 1:
            c = KTRI[dg * 2 + k]
            P = [(cs[i][0] * w, cs[i][1] * w, z[i]) for i in c]
            out.append(([cs[i] for i in c], sd.tri_slope(*P), max(p[2] for p in P) - min(p[2] for p in P),
                        [(cs[i], 'A' if ap[i] else 'S', round(z[i], 3)) for i in c], [ap[i] for i in c]))
    if verbose:
        print('      -> masks d0=%d d1=%d, diag %d: %d triangle(s)' % (m[0], m[1], dg, len(out)))
    return out


def all_cells(F, rule):
    tris = []
    for (x, y), r in REC.items():
        if r['cwritten'] and r['onstride'] and r['st'] == 1:
            tris += apron_cell(F, rule, x, y)
    return tris


def inner(t, m=13):
    return o[0] + m <= t[0] < o[0] + W - m and o[1] + m <= t[1] < o[1] + W - m


def outline_orphans(F, tris):
    """Snowy sheet texels beside a column over their top whose edge no apron triangle continues."""
    cover_edges = set()
    for c, sl, hgt, corners, aps in tris:
        sheets = [cc for cc, a in zip(c, aps) if not a]
        apr = [cc for cc, a in zip(c, aps) if a]
        for s in sheets:
            for a in apr:
                cover_edges.add((s, a))
    res = {'floor': [], 'roof': []}
    for t, r in REC.items():
        if not r['base'] or Dd.get(t, 0) <= thr or not inner(t):
            continue
        S = H[t] + Dd[t]
        for (dx, dy) in N8[:4]:
            q = (t[0] + dx, t[1] + dy)
            if q not in H or H[q] <= S + SEAM or joined(H[t], S, H[q], H[q] + Dd[q], w):
                continue
            if (t, q) in cover_edges:
                continue
            res['floor' if H[t] < args.floor_max else 'roof'].append((t, q, S0[t] - H[t], F.get(q)))
    return res


OPENED_SKIN = 0.25    # r12: clean_weather_snow.comp kOpenedSkin
BASE_R = 4            # r12: 2 x kSnowBaseR (the widest feature the OPEN removes)
SUP_STEP = 0.1        # kSupStep
GLOBAL = D.meta['global_depth']


def opened_at_drop():
    """r12 (clean_weather_snow.comp, the cover kernel): a texel whose base the filter's OPEN lowered
    (its raw column top over the filtered base by > kOpenedSkin: inside a thin feature -- a foliage
    shell, a railing, a thin wall) with a DROP within kSnowBaseR texels (a base lower by more than
    kSupStep + the fill level: here the global depth, the level's upper bound) holds no depth."""
    out = set()
    for t, h in H.items():
        r = REC[t]
        if r[sd.F_RAWTOP] <= h + OPENED_SKIN:
            continue
        for dx, dy in N8[:4]:
            hit = False
            for k in range(1, BASE_R + 1):
                q = (t[0] + dx * k, t[1] + dy * k)
                rq = REC.get(q)
                if rq is None:
                    continue
                if q not in H or H[q] < h - SUP_STEP:
                    hit = True
            if hit:
                out.add(t)
                break
    return out


def drop_skirts():
    """Snowy sheet texels beside a LOWER base they cannot join, or no base: a skirt (SKIRT_DROP)."""
    res = {'floor': [], 'roof': []}
    for t, h in H.items():
        if Dd.get(t, 0) <= thr or not inner(t):
            continue
        S = h + Dd[t]
        for dx, dy in N8[:4]:
            q = (t[0] + dx, t[1] + dy)
            if q not in REC:
                continue
            if q in H and (H[q] >= h or joined(h, S, H[q], H[q] + Dd[q], w)):
                continue
            res['floor' if h < args.floor_max else 'roof'].append((t, q, Dd[t]))
            break
    return res


CANOPY_LIFT = 1.0     # r13: clean_weather_snow.comp kCanopyLift


def canopy_over_ring():
    """r13 (clean_weather_snow.comp, the cover kernel): a texel whose filtered base stands more than
    kCanopyLift over the highest base of the outer ring's 2 x 2 taps there (the coarse map's filtered
    surface: 0.5 m texels, opened at 1.5 m -- it has no surface at this height here: a crown, a tier, a
    post's cap narrower than its opening) holds no depth."""
    I, R = D.meta['inner'], D.meta['ring']
    out = set()
    if not R.get('on') or not R['res']:
        return out
    for t, h in H.items():
        x = I['x0'] + (t[0] + 0.5) * I['extent'] / I['res'] + (I['top'] - h) * I['shear'][0]
        y = I['y0'] + (t[1] + 0.5) * I['extent'] / I['res'] + (I['top'] - h) * I['shear'][1]
        fx = (x - (R['top'] - h) * R['shear'][0] - R['x0']) / R['extent'] * R['res'] - 0.5
        fy = (y - (R['top'] - h) * R['shear'][1] - R['y0']) / R['extent'] * R['res'] - 0.5
        i0, j0 = math.floor(fx), math.floor(fy)
        best = None
        for k in range(4):
            r = D.texel(1, i0 + (k & 1), j0 + (k >> 1))
            if r is None or not r['written'] or not r['base']:
                continue
            best = r[sd.F_H] if best is None else max(best, r[sd.F_H])
        if best is not None and h > best + CANOPY_LIFT:
            out.add(t)
    return out


rules = args.rules.split(',')
variants = rules + ['r12', 'r13']
Dd0, S00 = dict(Dd), dict(S0)
opened = opened_at_drop()
canopy = canopy_over_ring()
fields, depths = {}, {}
for rl in variants:
    gone = opened if rl == 'r12' else (opened | canopy if rl == 'r13' else set())
    Dd = {t: (0.0 if t in gone else d) for t, d in Dd0.items()}
    S0 = {t: (H[t] if t in gone else z) for t, z in S00.items()}
    fields[rl] = dilate('r11' if rl in ('r12', 'r13') else rl)
    depths[rl] = (Dd, S0)
print('r13: + %d snowy texels over the ring\'s surface by more than %.1f m lose their depth (%d of them under %.0f m)'
      % (sum(1 for t in canopy - opened if Dd0.get(t, 0) > thr), CANOPY_LIFT,
         sum(1 for t in canopy - opened if Dd0.get(t, 0) > thr and H[t] < args.floor_max), args.floor_max))
print('r12: %d snowy texels inside a filter-opened feature at a drop lose their depth (%d of them under %.0f m)'
      % (sum(1 for t in opened if Dd0.get(t, 0) > thr), sum(1 for t in opened if Dd0.get(t, 0) > thr and H[t] < args.floor_max),
         args.floor_max))
cmpd = [t for t in H if inner(t)]
print('validation (which rules reproduce the dump\'s own field, %d interior texels):' % len(cmpd))
for rl in variants:
    F = fields[rl]
    agree = sum(1 for t in cmpd if (t in F) == (t in ZA) and (t not in F or abs(F[t][0] - ZA[t]) < 2e-3))
    print('   %-4s %d' % (rl, agree))
lim = math.degrees(math.atan(tanR)) + 10
for rl in variants:
    F = fields[rl]
    Dd, S0 = depths[rl]
    crule = 'r11' if rl in ('r12', 'r13') else rl
    tris = all_cells(F, crule)
    steep = [t for t in tris if t[1] is not None and t[1] > lim and t[2] > 0.05]
    orph = outline_orphans(F, tris)
    drops = drop_skirts()
    raised = sum(1 for t, h in H.items() if Dd.get(t, 0) > thr and D.terrain_at(0, *t) is not None
                 and h - D.terrain_at(0, *t) > 1.0)
    print('\n%s: snowy texels more than 1 m over the terrain: %d' % (rl, raised))
    print('%s: %d apron triangles, %d steeper than %.0f deg and > 5 cm (max %.0f deg, tallest %.2f m)'
          % (rl, len(tris), len(steep), lim, max((t[1] for t in tris if t[1] is not None), default=0),
             max((t[2] for t in steep), default=0)))
    for lev in ('floor', 'roof'):
        hs = sorted(e[2] for e in orph[lev])
        big = [e for e in orph[lev] if e[2] > 0.05]
        dh = sorted(e[2] for e in drops[lev])
        print('   %s level: outline edges without an apron %d (%d > 5 cm; tallest %.2f m); drop skirts %d (%d > 5 cm; tallest %.2f m)'
              % (lev, len(orph[lev]), len(big), hs[-1] if hs else 0, len(dh), sum(1 for v in dh if v > 0.05), dh[-1] if dh else 0))
        for t, q, hgt, fq in sorted(big, key=lambda e: -e[2])[:3]:
            print('      %s -> %s skirt %.2f m; the cover column base %.2f, its field %s'
                  % (t, q, hgt, H[q], ('z %.2f floor %.2f src depth %.2f' % fq) if fq else 'none'))
    for t in sorted(steep, key=lambda t: -t[2])[:3]:
        print('   steep %s %.0f deg %.2f m  %s' % (t[0][0], t[1], t[2], t[3]))
for spec in args.cells:
    x, y = [int(v) for v in spec.split(',')]
    for rl in variants:
        Dd, S0 = depths[rl]
        print('\ncell (%d, %d) under %s:' % (x, y, rl))
        apron_cell(fields[rl], 'r11' if rl in ('r12', 'r13') else rl, x, y, verbose=True)
