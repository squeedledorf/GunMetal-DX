#!/usr/bin/env python3
"""#683 early-gain CPU simulation (scripts/acoustics; earlygain_cases.py runs the reference table): mirrors clean_audio_rt.comp's bounce pass for a
SOURCE-origin set whose receiver is the LISTENER (diffuse rain shadow rays + specular
near-passes), and the CPU's early-gain formula in llcleanacoustics.cpp.

ratio_b = sum(arrivals with t <= D/c + window) / rays / E_free_b,
E_free_b = rr^2 / (4 D^2) * exp(-air_b D)          (same receiver sphere as the deposits)
level   = sqrt(g0^2 + max(g^2 - g0^2, ratio_mid))  (g = today's direct gain, g0 = without diffraction)
"""
import numpy as np, sys, math

C = 343.0
AIR = np.array([0.0011, 0.005, 0.04]) / 4.3429448
UNMOD = 0.10
# class: absorption low/mid/high (+ kBounceUnmodelled), scattering
METAL = (np.array([0.03, 0.03, 0.04]) + UNMOD, 0.05)
STONE = (np.array([0.02, 0.03, 0.05]) + UNMOD, 0.10)
WOOD = (np.array([0.12, 0.07, 0.09]) + UNMOD, 0.15)
TERRAIN = (np.array([0.15, 0.25, 0.40]) + UNMOD, 0.50)
HARD = (np.array([0.03, 0.03, 0.03]) + UNMOD, 0.05)   # gold PBR: hint pulls to hard / polished


class Scene:
    def __init__(self):
        self.tris = []; self.mat = []; self.terrain = []

    def tri(self, a, b, c, mat, terrain=False):
        self.tris.append((a, b, c)); self.mat.append(mat); self.terrain.append(terrain)

    def quad(self, a, b, c, d, mat, terrain=False):
        self.tri(a, b, c, mat, terrain); self.tri(a, c, d, mat, terrain)

    def build(self):
        if not self.tris:
            return
        t = np.array(self.tris, dtype=np.float64)
        self.v0, self.e1, self.e2 = t[:, 0], t[:, 1] - t[:, 0], t[:, 2] - t[:, 0]
        n = np.cross(self.e1, self.e2)
        self.n = n / np.linalg.norm(n, axis=1, keepdims=True)
        self.abs = np.array([m[0] for m in self.mat]); self.scat = np.array([m[1] for m in self.mat])
        self.is_terrain = np.array(self.terrain)

    def hit(self, o, d, tmin, tmax):
        """closest hit per ray: (t, tri) with t=-1 for a miss"""
        M = o.shape[0]
        best_t = np.full(M, np.inf); best_i = np.full(M, -1)
        if len(self.tris) == 0:
            return np.full(M, -1.0), best_i
        CH = 2048
        for s in range(0, M, CH):
            oo, dd = o[s:s + CH, None, :], d[s:s + CH, None, :]
            p = np.cross(dd, self.e2[None])
            det = np.einsum('ijk,jk->ij', p, self.e1)
            ok = np.abs(det) > 1e-12
            inv = np.where(ok, 1.0 / np.where(ok, det, 1.0), 0.0)
            tv = oo - self.v0[None]
            u = np.einsum('ijk,ijk->ij', tv, p) * inv
            q = np.cross(tv, self.e1[None])
            v = np.einsum('ijk,ijk->ij', dd, q) * inv
            t = np.einsum('ijk,jk->ij', q, self.e2) * inv
            valid = ok & (u >= 0) & (v >= 0) & (u + v <= 1) & (t > tmin[s:s + CH, None]) & (t < tmax[s:s + CH, None])
            t = np.where(valid, t, np.inf)
            i = np.argmin(t, axis=1)
            bt = t[np.arange(t.shape[0]), i]
            best_t[s:s + CH] = bt; best_i[s:s + CH] = np.where(np.isfinite(bt), i, -1)
        return np.where(np.isfinite(best_t), best_t, -1.0), best_i


def ground(sc, z=0.0, R=200.0, mat=TERRAIN, terrain=True):
    sc.quad((-R, -R, z), (R, -R, z), (R, R, z), (-R, R, z), mat, terrain)


def box(sc, lo, hi, mat):
    x0, y0, z0 = lo; x1, y1, z1 = hi
    P = lambda x, y, z: (x, y, z)
    sc.quad(P(x0, y0, z0), P(x1, y0, z0), P(x1, y1, z0), P(x0, y1, z0), mat)  # floor
    sc.quad(P(x0, y0, z1), P(x0, y1, z1), P(x1, y1, z1), P(x1, y0, z1), mat)  # ceiling
    sc.quad(P(x0, y0, z0), P(x0, y1, z0), P(x0, y1, z1), P(x0, y0, z1), mat)
    sc.quad(P(x1, y0, z0), P(x1, y0, z1), P(x1, y1, z1), P(x1, y1, z0), mat)
    sc.quad(P(x0, y0, z0), P(x0, y0, z1), P(x1, y0, z1), P(x1, y0, z0), mat)
    sc.quad(P(x0, y1, z0), P(x1, y1, z0), P(x1, y1, z1), P(x0, y1, z1), mat)


def horn(sc, base, axis_len=12.0, r0=0.75, r1=4.0, seg=32, rings=8, mat=HARD, cap=True):
    """a hollow cone along +x from base (throat) to base + axis_len (mouth)"""
    bx, by, bz = base
    for k in range(rings):
        x0 = axis_len * k / rings; x1 = axis_len * (k + 1) / rings
        ra = r0 + (r1 - r0) * k / rings; rb = r0 + (r1 - r0) * (k + 1) / rings
        for j in range(seg):
            a0 = 2 * math.pi * j / seg; a1 = 2 * math.pi * (j + 1) / seg
            p = lambda x, r, a: (bx + x, by + r * math.cos(a), bz + r * math.sin(a))
            sc.quad(p(x0, ra, a0), p(x1, rb, a0), p(x1, rb, a1), p(x0, ra, a1), mat)
    if cap:
        for j in range(seg):
            a0 = 2 * math.pi * j / seg; a1 = 2 * math.pi * (j + 1) / seg
            sc.tri((bx, by, bz), (bx, by + r0 * math.cos(a1), bz + r0 * math.sin(a1)),
                   (bx, by + r0 * math.cos(a0), bz + r0 * math.sin(a0)), mat)


def fib(n, rng):
    j = np.arange(n) + rng.random()
    z = 1 - 2 * j / n
    ph = 2 * math.pi * ((np.arange(n) * 0.61803398875 + rng.random()) % 1.0)
    s = np.sqrt(np.maximum(0, 1 - z * z))
    return np.stack([s * np.cos(ph), s * np.sin(ph), z], 1)


def trace(sc, S, R, rays, bounces, rr, window, rng, tmin0=0.05, skip_floor=True, rng_m=60.0, origin_bug=False):
    """origin_bug: reproduce the shader before the #683 fix (rays left the receiver R, not S)."""
    """one set: returns arrival energy per band within the early window (ray units)"""
    S = np.asarray(S, float); R = np.asarray(R, float)
    D = np.linalg.norm(R - S)
    t_end = D / C + window
    d = fib(rays, rng)
    p = np.repeat((R if origin_bug else S)[None], rays, 0)
    E = np.ones((rays, 3)); path = np.zeros(rays); spec = np.zeros(rays, bool)
    alive = np.ones(rays, bool); tmin = np.full(rays, tmin0)
    floor1 = np.zeros(rays, bool)
    out = np.zeros(3); out_floor = np.zeros(3); n_nee = 0; n_spec = 0
    global LAST_SPEC; LAST_SPEC = np.zeros(3)
    for k in range(1, bounces + 1):
        idx = np.nonzero(alive)[0]
        if idx.size == 0: break
        th, ti = sc.hit(p[idx], d[idx], tmin[idx], np.full(idx.size, rng_m))
        seg_end = np.where(th >= 0, th, rng_m)
        # specular near-pass (order k-1)
        if k >= 2:
            sp = spec[idx]
            tc = np.clip(np.einsum('ij,ij->i', R[None] - p[idx], d[idx]), 0, seg_end)
            near = sp & (np.linalg.norm(p[idx] + d[idx] * tc[:, None] - R[None], axis=1) < rr)
            arr_t = (path[idx] + tc) / C
            ok = near & (arr_t <= t_end)
            if skip_floor and k == 2:
                fl = ok & floor1[idx]
                out_floor += (E[idx][fl] * np.exp(-AIR[None] * tc[fl, None])).sum(0)
                ok &= ~floor1[idx]
            _sp = (E[idx][ok] * np.exp(-AIR[None] * tc[ok, None])).sum(0); out += _sp; LAST_SPEC += _sp
            n_spec += int(near.sum())
        miss = th < 0
        alive[idx[miss]] = False
        idx = idx[~miss]; th = th[~miss]; ti = ti[~miss]
        if idx.size == 0: break
        path[idx] += th
        E[idx] *= np.exp(-AIR[None] * th[:, None])
        P = p[idx] + d[idx] * th[:, None]
        n = sc.n[ti].copy()
        flip = np.einsum('ij,ij->i', n, d[idx]) > 0
        n[flip] = -n[flip]
        E[idx] *= 1 - sc.abs[ti]
        scat = sc.scat[ti]
        if k == 1:
            floor1[idx] = sc.is_terrain[ti] | (n[:, 2] > 0.9)
        # diffuse rain to R (shadow ray always: receiver != origin)
        toR = R[None] - P
        dR = np.linalg.norm(toR, axis=1)
        u = toR / np.maximum(dR, 1e-6)[:, None]
        cosT = np.einsum('ij,ij->i', n, u)
        cand = (dR > 0.05) & (cosT > 0) & (scat > 0)
        if cand.any():
            ci = np.nonzero(cand)[0]
            o2 = P[ci] + n[ci] * 0.02
            sh, _ = sc.hit(o2, u[ci], np.full(ci.size, 0.02), dR[ci] - 0.05)
            vis = sh < 0
            ci = ci[vis]
            frac = scat[ci] * cosT[ci] * np.minimum(rr * rr / dR[ci] ** 2, 0.5)
            arr_t = (path[idx[ci]] + dR[ci]) / C
            ok = arr_t <= t_end
            dep = E[idx[ci]] * (frac * 1.0)[:, None] * np.exp(-AIR[None] * dR[ci, None])
            if skip_floor and k == 1:
                fl = ok & floor1[idx[ci]]
                out_floor += dep[fl].sum(0)
                ok &= ~floor1[idx[ci]]
            out += dep[ok].sum(0)
            n_nee += int(ci.size)
        # next direction
        ru = rng.random(idx.size)
        dif = ru < scat
        nd = d[idx] - 2 * np.einsum('ij,ij->i', d[idx], n)[:, None] * n
        if dif.any():
            m = np.nonzero(dif)[0]
            nn = n[m]
            ax = np.where(np.abs(nn[:, 2:3]) < 0.9, np.array([[0, 0, 1.0]]), np.array([[1.0, 0, 0]]))
            tu = np.cross(nn, ax); tu /= np.linalg.norm(tu, axis=1, keepdims=True)
            tv = np.cross(nn, tu)
            u1 = rng.random(m.size); u2 = rng.random(m.size)
            rad = np.sqrt(u1); ph = 2 * math.pi * u2
            v = tu * (rad * np.cos(ph))[:, None] + tv * (rad * np.sin(ph))[:, None] + nn * np.sqrt(1 - u1)[:, None]
            nd[m] = v / np.linalg.norm(v, axis=1, keepdims=True)
        sp_new = ~dif
        d[idx] = nd; spec[idx] = sp_new
        p[idx] = P + n * 0.02
        tmin[idx] = 0.01
    return out, out_floor, D, n_nee, n_spec


def recv_radius(D):
    return min(max(0.15 * D, 0.5), 2.0, 0.5 * D)


def early_ratio(sc, S, R, rays, bounces, window, rng, skip_floor=True, rr=None):
    out, fl, D, nn, ns = trace(sc, S, R, rays, bounces, rr or recv_radius(np.linalg.norm(np.subtract(R, S))), window, rng,
                               skip_floor=skip_floor)
    rr = rr or recv_radius(D)
    efree = rr * rr / (4 * D * D) * np.exp(-AIR * D)
    return out / rays / efree, fl / rays / efree, D, nn / rays, ns / rays


def level_db(ratio_mid, g=1.0, g0=1.0, cap_db=9.0):
    e = g0 * g0 + max(g * g - g0 * g0, ratio_mid)
    L = min(math.sqrt(e), g * 10 ** (cap_db / 20))
    return 20 * math.log10(L / g), 20 * math.log10(L)


def run(name, sc, S, R, bounces=4, window=0.05, rays_total=16384, budget_rays=128, answers_per_tau=4, seed=1,
        g=1.0, g0=1.0, skip_floor=True):
    sc.build()
    rng = np.random.default_rng(seed)
    # converged value
    r, fl, D, nn, ns = early_ratio(sc, S, R, rays_total, bounces, window, rng, skip_floor)
    # noise at the real budget: accumulated over ~ answers_per_tau answers of budget_rays (exponential forgetting ~ tau)
    vals = []
    for trial in range(24):
        num = np.zeros(3); den = 0.0
        for a in range(answers_per_tau * 3):
            ri, _, _, _, _ = early_ratio(sc, S, R, budget_rays, bounces, window, rng, skip_floor)
            k = math.exp(-1.0 / answers_per_tau)
            num = num * k + ri * budget_rays; den = den * k + budget_rays
        vals.append(level_db((num / den)[1], g, g0)[0])
    vals = np.array(vals)
    hf = math.sqrt((g * g + r[2]) / (g * g + r[1])) if True else 1
    rel, absdb = level_db(r[1], g, g0)
    print(f"{name:42s} D={D:5.1f} m  ratio low/mid/high {r[0]:.2f}/{r[1]:.2f}/{r[2]:.2f}  "
          f"floor1 {fl[1]:.2f}  gain {rel:+.2f} dB (abs {absdb:+.2f})  hf x{min(hf,1):.2f}  "
          f"@128 rays: {vals.mean():+.2f} +- {vals.std():.2f} dB  nee/ray {nn:.2f} spec/ray {ns:.3f}")
    return rel


if __name__ == "__main__":
    which = sys.argv[1:] or ["all"]
    H = 4.0   # horn axis height
    base = (0.0, 0.0, H)
    S_in = (2.0, 0.0, H)   # 2 m in from the throat, on axis
    for skip in (True, False):
        print(f"--- skip first-order floor/terrain reflection: {skip}")
        def hornscene(cap=True):
            sc = Scene(); ground(sc); horn(sc, base, cap=cap); return sc
        run("horn, on axis 8 m before mouth", hornscene(), S_in, (20.0, 0.0, H), skip_floor=skip)
        run("horn, on axis 25 m before mouth", hornscene(), S_in, (37.0, 0.0, H), skip_floor=skip)
        run("horn, open throat, on axis 8 m", hornscene(False), S_in, (20.0, 0.0, H), skip_floor=skip)
        run("horn, 45 deg off axis (8 m before, 8 m side)", hornscene(), S_in, (20.0, 8.0, H), skip_floor=skip)
        run("horn, 90 deg (beside the mouth, 10 m out)", hornscene(), S_in, (12.0, 10.0, H), skip_floor=skip)
        # behind: the wall blocks the direct line -> occluded (today's g ~0.56, no diffraction)
        run("horn, behind the throat 8 m (occluded)", hornscene(), S_in, (-8.0, 0.0, H), skip_floor=skip, g=0.56, g0=0.56)
        sc = Scene(); box(sc, (0, 0, 0), (5, 4, 2.8), STONE)
        run("small stone room 5x4x2.8, 3 m apart", sc, (1.0, 2.0, 1.4), (4.0, 2.0, 1.6), skip_floor=skip)
        sc = Scene(); box(sc, (0, 0, 0), (5, 4, 2.8), WOOD)
        run("small wood room 5x4x2.8, 3 m apart", sc, (1.0, 2.0, 1.4), (4.0, 2.0, 1.6), skip_floor=skip)
        sc = Scene(); box(sc, (0, 0, 0), (12, 9, 5), STONE)
        run("stone hall 12x9x5, 8 m apart", sc, (2.0, 4.5, 1.5), (10.0, 4.5, 1.7), skip_floor=skip)
        sc = Scene(); ground(sc)
        run("open field (terrain), 10 m", sc, (0, 0, 1.0), (10.0, 0, 1.7), skip_floor=skip)
        sc = Scene(); ground(sc, mat=WOOD, terrain=False)
        run("open prim platform (wood), 10 m", sc, (0, 0, 1.0), (10.0, 0, 1.7), skip_floor=skip)
        sc = Scene(); pass
        run("free field (nothing)", Scene() if False else sc, (0, 0, 1.0), (10.0, 0, 1.7), skip_floor=skip)
        # source behind a wall: wall 6 m wide x 4 m tall between, on terrain; today's g = 0.56
        sc = Scene(); ground(sc); sc.quad((5, -3, 0), (5, 3, 0), (5, 3, 4), (5, -3, 4), STONE)
        run("behind a 6x4 m wall (occluded), 10 m", sc, (3.0, 0, 1.0), (13.0, 0, 1.7), g=0.56, g0=0.56, skip_floor=skip)
        sc = Scene(); ground(sc); sc.quad((5, -3, 0), (5, 3, 0), (5, 3, 4), (5, -3, 4), STONE)
        run("  same, with a #858 path (g=0.8, g0=0.56)", sc, (3.0, 0, 1.0), (13.0, 0, 1.7), g=0.8, g0=0.56, skip_floor=skip)
        # sealed: source in a closed stone room, listener outside
        sc = Scene(); ground(sc); box(sc, (0, -3, 0.01), (6, 3, 3), STONE)
        run("sealed stone room, listener outside 6 m", sc, (3.0, 0, 1.4), (12.0, 0, 1.7), g=0.56, g0=0.56, skip_floor=skip)
