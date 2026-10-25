#!/usr/bin/env python3
"""#683 early gain: the reference cases end to end (the shader's estimator in earlygain_sim.trace,
the CPU's level rule in applied()). Prints one table row per case: converged early/direct ratio
(mid), the dB the CPU would apply, and the spread at the real ray budget.

usage: earlygain_cases.py [--rays N] [--budget N] [--bounces B] [--window MS] [--fast]
"""
import sys, os, math, argparse
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import numpy as np
import earlygain_sim as E
from earlygain_sim import Scene, ground, box, STONE, WOOD, HARD
from owner_horn import hollow_horn

CAP_DB, SHARE = 9.0, 0.9


def applied(r_mid, cover, cap_db=CAP_DB):
    """CPU level rule for an unoccluded source (g = g0 = 1): E = 1 + w r, w = 1 - share x cover."""
    w = 1.0 - SHARE * cover
    m = min(math.sqrt(1.0 + w * r_mid), 10 ** (cap_db / 20))
    return 20 * math.log10(m)


def ratio(sc, S, R, rays, bounces, window, rng, rr=None, origin_bug=False):
    D = float(np.linalg.norm(np.subtract(R, S)))
    rr = rr or E.recv_radius(D)
    out, fl, D, nn, ns = E.trace(sc, S, R, rays, bounces, rr, window, rng, origin_bug=origin_bug)
    ef = rr * rr / (4 * D * D) * np.exp(-E.AIR * D)
    return out / rays / ef, D, nn / rays, ns / rays


def case(name, sc, S, R, cover, a, rng, expect=""):
    sc.build()
    r, D, nn, ns = ratio(sc, S, R, a.rays, a.bounces, a.window, rng)
    db = applied(r[1], cover)
    rb = ratio(sc, S, R, a.rays // 2, a.bounces, a.window, rng, origin_bug=True)[0]   # before the fix
    # at the real budget: answers of `budget` rays, kEarlyAccTau 0.5 s forgetting at ~10 answers/s
    vals = []
    for t in range(a.trials):
        num = 0.0; den = 0.0
        for k in range(10):
            ri = ratio(sc, S, R, a.budget, a.bounces, a.window, rng)[0][1]
            f = math.exp(-0.1 / 0.5)
            num = num * f + ri * a.budget; den = den * f + a.budget
        vals.append(applied(num / den, cover))
    vals = np.array(vals)
    print(f"| {name:52s} | {D:5.1f} | {rb[1]:7.2f} ({applied(rb[1], cover):+4.1f}) | {r[1]:7.2f} | {cover:.1f} | {db:+5.1f} | {vals.mean():+5.1f} +- {vals.std():.1f} | {expect} |",
          flush=True)


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--rays", type=int, default=8192)
    ap.add_argument("--budget", type=int, default=128)
    ap.add_argument("--bounces", type=int, default=3)
    ap.add_argument("--window", type=float, default=50.0)
    ap.add_argument("--trials", type=int, default=6)
    a = ap.parse_args()
    a.window /= 1000.0
    rng = np.random.default_rng(5)
    print(f"bounces {a.bounces}, window {a.window*1000:.0f} ms, cap +{CAP_DB} dB, converged {a.rays} rays, budget {a.budget} rays/answer")
    print("| case | D m | before fix: e/d (dB) | early/direct mid | cover | dB | dB at budget | expected |")
    print("|---|---|---|---|---|---|---|---|")

    def horn():
        sc = Scene(); ground(sc); H = hollow_horn(sc, taper=0.5); return sc, H
    sc, H = horn()
    case("owner horn: src 1 m in narrow end, L mouth plane", sc, (1, 0, H), (25.15, 0, H), 0.0, a, rng, ">= +6")
    sc, H = horn()
    case("owner horn: src 1 m in, L 2 m outside the mouth", sc, (1, 0, H), (27.1, 0, H), 0.0, a, rng, ">= +6")
    sc, H = horn()
    case("owner horn: src 1 m in, L 20 m beyond the mouth", sc, (1, 0, H), (46.0, 0, H), 0.0, a, rng, ">= +6")
    sc, H = horn()
    case("owner horn: src on avatar (1.3 m over the floor)", sc, (1, 0, H - 2.55 + 1.3), (27.1, 0, H + 1), 0.0, a, rng, ">= +6")
    sc, H = horn()
    case("owner horn reversed: src at mouth, L 1 m in back", sc, (25.15, 0, H), (1, 0, H), 0.0, a, rng, "clear boost")
    sc, H = horn()
    case("owner horn: L 15 m to the side of the mouth", sc, (1, 0, H), (27.0, 15.0, H), 0.0, a, rng, "small")

    def horn_wood():
        sc = Scene(); ground(sc); H = hollow_horn(sc, taper=0.5, mat=WOOD); return sc, H
    sc, H = horn_wood()
    case("owner horn, NOT shiny (wood class), L 2 m out", sc, (1, 0, H), (27.1, 0, H), 0.0, a, rng, ">= +6")

    sc = Scene(); ground(sc)
    case("open field (terrain), 20 m", sc, (0, 0, 1.0), (20, 0, 1.7), 0.0, a, rng, "0")
    sc = Scene(); ground(sc, mat=WOOD, terrain=False)
    case("open wooden platform, 20 m", sc, (0, 0, 1.0), (20, 0, 1.7), 0.0, a, rng, "~0")

    def street():
        sc = Scene(); ground(sc)
        box(sc, (-40, 4, 0), (40, 14, 10), STONE)     # buildings both sides of an 8 m street
        box(sc, (-40, -14, 0), (40, -4, 10), STONE)
        return sc
    case("street 8 m wide, stone fronts, both on it, 20 m", street(), (0, 0, 1.2), (20, 1, 1.7), 0.0, a, rng, "<= +3")
    def wall_near_L():
        sc = Scene(); ground(sc); sc.quad((25, -10, 0), (25, 10, 0), (25, 10, 8), (25, -10, 8), STONE); return sc
    case("open, L 1 m in front of a 20x8 stone wall, 24 m", wall_near_L(), (0, 0, 1.2), (24, 0, 1.7), 0.0, a, rng, "<= +3")
    def corner():
        sc = Scene(); ground(sc); box(sc, (21, 1.5, 0), (30, 12, 8), STONE); return sc
    case("open, L 1 m off a building corner, 22 m", corner(), (0, 0, 1.2), (20, 0.5, 1.7), 0.0, a, rng, "<= +3")
    def plaza():
        sc = Scene(); ground(sc)
        for (x, y) in ((-15, 15), (10, 18), (30, -12), (-5, -20)):
            box(sc, (x, y, 0), (x + 8, y + 8, 6), WOOD)
        return sc
    case("plaza with four wooden houses, 25 m", plaza(), (0, 0, 1.2), (25, 3, 1.7), 0.0, a, rng, "<= +2")

    def room_door():
        # a 5 x 4 x 2.8 m stone room, open 1.2 m doorway in its +x wall; the source outside
        sc = Scene(); ground(sc)
        x0, y0, x1, y1, z1 = 0, 0, 5, 4, 2.8
        box_walls = [((x0, y0, 0), (x0, y1, 0), (x0, y1, z1), (x0, y0, z1)),
                     ((x0, y0, 0), (x1, y0, 0), (x1, y0, z1), (x0, y0, z1)),
                     ((x0, y1, 0), (x1, y1, 0), (x1, y1, z1), (x0, y1, z1)),
                     ((x0, y0, z1), (x1, y0, z1), (x1, y1, z1), (x0, y1, z1)),
                     ((x1, y0, 0), (x1, 1.4, 0), (x1, 1.4, z1), (x1, y0, z1)),
                     ((x1, 2.6, 0), (x1, y1, 0), (x1, y1, z1), (x1, 2.6, z1)),
                     ((x1, 1.4, 2.1), (x1, 2.6, 2.1), (x1, 2.6, z1), (x1, 1.4, z1))]
        for q in box_walls:
            sc.quad(*q, STONE)
        return sc
    # listener inside off the door line (occluded direct: today's g ~0.56); the source 38 m out
    sc = room_door()
    sc.build()
    r, D, _, _ = ratio(sc, (43, 9, 1.2), (2, 3, 1.6), a.rays, a.bounces, a.window, rng)
    rb = ratio(sc, (43, 9, 1.2), (2, 3, 1.6), a.rays // 2, a.bounces, a.window, rng, origin_bug=True)[0]
    def occ_db(rm, g=0.56):
        e = g * g + max(rm, 0.0)
        return 20 * math.log10(min(math.sqrt(e) / g, 10 ** (CAP_DB / 20)))
    print(f"| {'L inside a stone room, src 38 m out (occluded, g 0.56)':52s} | {D:5.1f} | {rb[1]:7.2f} ({occ_db(rb[1]):+4.1f}) | "
          f"{r[1]:7.2f} | 0.0 | {occ_db(r[1]):+5.1f} | - | small |", flush=True)

    sc = Scene(); box(sc, (0, 0, 0), (5, 4, 2.8), STONE)
    case("small stone room 5x4x2.8, 3 m (cover 1)", sc, (1, 2, 1.4), (4, 2, 1.6), 1.0, a, rng, "+1..3")
    sc = Scene(); box(sc, (0, 0, 0), (5, 4, 2.8), WOOD)
    case("small wood room 5x4x2.8, 3 m (cover 1)", sc, (1, 2, 1.4), (4, 2, 1.6), 1.0, a, rng, "+1..3")
    sc = Scene(); box(sc, (0, 0, 0), (12, 9, 5), STONE)
    case("stone hall 12x9x5, 8 m (cover 1)", sc, (2, 4.5, 1.5), (10, 4.5, 1.7), 1.0, a, rng, "+1..3")


if __name__ == "__main__":
    main()
