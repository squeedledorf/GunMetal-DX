import sys, os, math
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import numpy as np
from earlygain_sim import Scene, ground, early_ratio, HARD

def hollow_horn(sc, L=25.15, r1=5.675, taper=0.5, hollow=0.9, seg=32, rings=16, H=6.5, mat=HARD):
    """SL hollow tapered cylinder lying along +x: narrow end x=0 (radius r1*(1-taper)), mouth x=L (r1).
    Inner + outer walls, annular end caps (open hollow at both ends)."""
    r0 = r1 * (1 - taper)
    P = lambda x, r, a: (x, r * math.cos(a), H + r * math.sin(a))
    for k in range(rings):
        x0, x1 = L * k / rings, L * (k + 1) / rings
        ra = r0 + (r1 - r0) * k / rings; rb = r0 + (r1 - r0) * (k + 1) / rings
        for j in range(seg):
            a0, a1 = 2 * math.pi * j / seg, 2 * math.pi * (j + 1) / seg
            for f in (1.0, hollow):
                sc.quad(P(x0, ra * f, a0), P(x1, rb * f, a0), P(x1, rb * f, a1), P(x0, ra * f, a1), mat)
    for x, r in ((0.0, r0), (L, r1)):
        for j in range(seg):
            a0, a1 = 2 * math.pi * j / seg, 2 * math.pi * (j + 1) / seg
            sc.quad(P(x, r * hollow, a0), P(x, r, a0), P(x, r, a1), P(x, r * hollow, a1), mat)
    return H

if __name__ == "__main__":
    rng = np.random.default_rng(1)
    for taper in (0.4, 0.7, 0.97):
        sc = Scene(); ground(sc); H = hollow_horn(sc, taper=taper); sc.build()
        S = (1.0, 0, H)
        for lx in (25.15, 28.1, 46.0):
            for B in (3, 8):
                r, fl, D, nn, ns = early_ratio(sc, S, (lx, 0, H), 8192, B, 0.05, rng)
                print(f"taper {taper} L at x={lx:5.1f} D {D:5.1f} B{B}: ratio {r[1]:7.2f} nee/ray {nn:.2f} spec/ray {ns:.3f}")
