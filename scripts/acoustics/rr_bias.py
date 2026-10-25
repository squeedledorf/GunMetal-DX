"""#683 early gain: receiver-radius bias next to surfaces (the sphere straddling a wall counts a
reflection's incoming and outgoing segments both). Ratio vs rr for a few listener positions."""
import sys, os
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import numpy as np
import earlygain_sim as E
from earlygain_sim import Scene, ground, box, STONE
from owner_horn import hollow_horn
from earlygain_cases import ratio

rng = np.random.default_rng(9)


def wall():
    sc = Scene(); ground(sc); sc.quad((25, -10, 0), (25, 10, 0), (25, 10, 8), (25, -10, 8), STONE); sc.build(); return sc


def street():
    sc = Scene(); ground(sc)
    box(sc, (-40, 4, 0), (40, 14, 10), STONE); box(sc, (-40, -14, 0), (40, -4, 10), STONE); sc.build(); return sc


def horn():
    sc = Scene(); ground(sc); H = hollow_horn(sc, taper=0.5); sc.build(); return sc, H


cases = [("L 1 m before a wall", wall(), (0, 0, 1.2), (24, 0, 1.7)),
         ("L 0.5 m before a wall", wall(), (0, 0, 1.2), (24.5, 0, 1.7)),
         ("street, L 1 m off a facade", street(), (0, 0, 1.2), (20, 3, 1.7)),
         ("street, centre", street(), (0, 0, 1.2), (20, 0, 1.7))]
sc, H = horn()
cases.append(("horn reversed: L in back", sc, (25.15, 0, H), (1, 0, H)))
cases.append(("horn: L mouth plane", sc, (1, 0, H), (25.15, 0, H)))
for name, sc, S, R in cases:
    row = []
    for rr in (0.25, 0.5, 1.0, 2.0):
        r = ratio(sc, S, R, 8192, 3, 0.05, rng, rr=rr)[0][1]
        row.append(f"rr {rr:4.2f}: {r:6.2f}")
    print(f"{name:30s} " + "  ".join(row), flush=True)
