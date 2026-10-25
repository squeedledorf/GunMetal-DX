import sys, os, math
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import numpy as np, earlygain_sim as E
from owner_horn import hollow_horn
rng = np.random.default_rng(2)
sc = E.Scene(); E.ground(sc); H = hollow_horn(sc, taper=0.5); sc.build()
r0 = 5.675*0.5*0.9
for name, S, Rs in [("on-axis src", (1.0,0,H), [(25.15,0,H),(28.1,0,H),(46,0,H)]),
                    ("src on bottom wall, 1.3m up", (1.0,0,H-r0+1.3), [(25.15,0,H),(28.1,0,H+1),(46,0,H+2)]),
                    ("off-axis both", (1.0,1.0,H-0.8), [(25.15,-1.5,H+1),(28.1,2,H-1),(46,3,H)])]:
    for R in Rs:
        for B in (3,8):
            rr = E.recv_radius(np.linalg.norm(np.subtract(R,S)))
            out, fl, D, nn, ns = E.trace(sc, S, R, 8192, B, rr, 0.05, rng)
            ef = rr*rr/(4*D*D)*np.exp(-E.AIR*D)
            sp = E.LAST_SPEC/8192/ef
            tot = out/8192/ef
            out2, *_ = E.trace(sc, S, R, 8192, 8, rr, 1.0, rng)
            print(f"{name:28s} D {D:5.1f} B{B}: ratio {tot[1]:7.2f} (spec {sp[1]:6.2f}, nee {tot[1]-sp[1]:6.2f})  long-window B8 {out2[1]/8192/ef[1]:7.2f}", flush=True)
