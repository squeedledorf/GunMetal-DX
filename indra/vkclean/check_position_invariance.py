#!/usr/bin/env python3
"""gl_Position bit-identity check between SPIR-V variants of one vertex / mesh shader.

For each module: glslc -O -> spirv-opt (strip debug, merge-return, ssa-rewrite, -O) ->
spirv-dis --raw-id. Every OpStore into the Position builtin is located and its value's
def-use cone is hashed STRUCTURALLY (opcode, result type, operand hashes; constants by value;
variables by storage class + DescriptorSet / Binding / BuiltIn / Location; a phi by the condition
of its immediate dominator and the side each incoming value comes from; IDs never matter). Two variants compute gl_Position
identically iff the multisets of cone hashes match. Also prints the cone sizes.

usage: check_position_invariance.py <shader> <stage> "<defines A>" "<defines B>" [...more variants]
"""
import hashlib, os, re, subprocess, sys, tempfile

LABEL_DEPTH = int(os.environ.get("LABEL_DEPTH", "64"))
SHD = os.path.join(os.path.dirname(os.path.abspath(__file__)), "shaders")

def build(src, stage, defs):
    with tempfile.TemporaryDirectory() as td:
        a = os.path.join(td, "a.spv"); b = os.path.join(td, "b.spv")
        subprocess.run(["glslc", "--target-env=vulkan1.3", "-O", f"-fshader-stage={stage}", *defs.split(),
                        os.path.join(SHD, src), "-o", a], check=True)
        subprocess.run(["spirv-opt", "--target-env=vulkan1.3", "--strip-debug", "--merge-return",
                        "--ssa-rewrite", "-O", a, "-o", b], check=True)
        return subprocess.run(["spirv-dis", "--raw-id", "--no-header", b], check=True,
                              capture_output=True, text=True).stdout

def parse(dis):
    defs, decos, mdecos, stores, blocks, term = {}, {}, {}, [], {}, {}
    cur = None
    for line in dis.splitlines():
        line = line.split(";")[0].strip()
        if not line: continue
        m = re.match(r"(%\d+) = (Op\w+)(.*)", line)
        if m:
            rid, op, rest = m.group(1), m.group(2), m.group(3).split()
            defs[rid] = (op, rest)
            if op == "OpLabel":
                cur = rid; blocks[cur] = []
            elif cur: blocks[cur].append(rid)
            continue
        toks = line.split()
        op = toks[0]
        if op == "OpDecorate": decos.setdefault(toks[1], []).append(" ".join(toks[2:]))
        elif op == "OpMemberDecorate": mdecos.setdefault(toks[1], []).append(" ".join(toks[2:]))
        elif op == "OpStore": stores.append((toks[1], toks[2], cur))
        elif op in ("OpBranch", "OpBranchConditional", "OpSwitch", "OpReturn", "OpKill", "OpUnreachable",
                    "OpTerminateInvocation", "OpReturnValue"):
            term[cur] = toks
    blockof = {i: b for b, ids in blocks.items() for i in ids}
    return defs, decos, mdecos, stores, term, blockof

class Hasher:
    def __init__(self, mod):
        self.defs, self.decos, self.mdecos, self.stores, self.term, self.blockof = mod
        self.dom = None
        self.memo = {}; self.busy = set(); self.cone = set()
        self.preds = {}
        for blk, t in self.term.items():
            if t[0] == "OpBranch": self.preds.setdefault(t[1], []).append((blk, "br"))
            elif t[0] == "OpBranchConditional":
                self.preds.setdefault(t[2], []).append((blk, ("T", t[1])))
                self.preds.setdefault(t[3], []).append((blk, ("F", t[1])))
            elif t[0] == "OpSwitch":
                self.preds.setdefault(t[2], []).append((blk, ("default", t[1])))
                for k in range(3, len(t) - 1, 2):
                    self.preds.setdefault(t[k + 1], []).append((blk, ("case" + t[k], t[1])))
    def H(self, x):
        if not x.startswith("%"): return x       # literal
        if x in self.memo: return self.memo[x]
        if x in self.busy: return "cycle"
        self.busy.add(x)
        self.cone.add(x)
        op, rest = self.defs.get(x, ("?", []))
        if op == "OpLabel":
            h = ("label", self.lab(x, LABEL_DEPTH))
        elif op == "OpVariable":
            h = ("var", tuple(rest[1:2]), self.H(rest[0]), tuple(sorted(self.decos.get(x, []))))
        elif op.startswith("OpType"):
            h = (op, tuple(self.H(r) for r in rest), tuple(sorted(self.mdecos.get(x, []))),
                 tuple(sorted(d for d in self.decos.get(x, []) if not d.startswith("Block"))))
        elif op == "OpPhi":
            h = self.phi(x, rest)
        else:
            h = (op,) + tuple(self.H(r) for r in rest)
        self.busy.discard(x)
        d = hashlib.sha1(str(h).encode()).hexdigest()[:16]
        self.memo[x] = d
        return d
    def lab(self, x, d):
        # A block label = the branch conditions (and sides) that reach it, LABEL_DEPTH
        # predecessor levels up: what selects a phi's incoming value. Control flow further
        # up (e.g. an unrelated octahedral-decode branch earlier in main) is not part of
        # the selection, so it is cut off.
        # Local selection signature: each incoming edge by its own branch condition and side;
        # an unconditional edge from a single-predecessor block follows that block's edge up;
        # an unconditional edge out of a merge block is just "merge" (what is selected there
        # is the incoming VALUE's business, hashed through its own operands).
        if d <= 0: return "*"
        parts = []
        for p, how in self.preds.get(x, []):
            if how != "br":
                parts.append((how[0], self.H(how[1])))
            elif len(self.preds.get(p, [])) == 1:
                parts.append(("via", self.lab(p, d - 1)))
            else:
                parts.append(("merge",))
        return tuple(sorted(map(str, parts)))
    def doms(self):
        # Iterative dominator sets over the whole module's blocks (one function after inlining).
        if self.dom is not None: return self.dom
        succ = {}
        for b, t in self.term.items():
            succ[b] = [t[1]] if t[0] == "OpBranch" else [t[2], t[3]] if t[0] == "OpBranchConditional" else \
                      ([t[2]] + t[4::2] if t[0] == "OpSwitch" else [])
        entry = list(self.term.keys())[0]
        reach, stack = set(), [entry]   # (unreachable blocks would poison the intersections)
        while stack:
            b = stack.pop()
            if b in reach: continue
            reach.add(b); stack.extend(succ.get(b, []))
        blocks = [b for b in self.term.keys() if b in reach]
        allb = set(blocks)
        dom = {b: set(allb) for b in blocks}
        dom[entry] = {entry}
        changed = True
        while changed:
            changed = False
            for b in blocks[1:]:
                ps = [q for q, _ in self.preds.get(b, []) if q in allb]
                nd = set.intersection(*[dom[q] for q in ps]) if ps else set()
                nd = nd | {b}
                if nd != dom[b]: dom[b] = nd; changed = True
        self.dom = dom
        return dom
    def idom(self, b):
        dom = self.doms()
        if b not in dom: return None
        cands = dom[b] - {b}
        for c in cands:
            if all(c == o or o in dom[c] for o in cands):   # c is dominated by every other candidate
                return c
        return None
    def phi(self, x, rest):
        # Selection semantics: the phi's block M, its immediate dominator D ending in a
        # conditional branch on c; each incoming value is tagged with the side of c whose
        # successor dominates (or is) its incoming block. Unrelated control flow elsewhere
        # (nested ifs inside an arm, earlier branches) does not enter the hash.
        M = self.blockof.get(x)
        D = self.idom(M) if M else None
        t = self.term.get(D) if D else None
        pairs = [(rest[i], rest[i + 1]) for i in range(1, len(rest), 2)]
        if t and t[0] == "OpBranchConditional":
            dom = self.doms()
            c, tT, tF = t[1], t[2], t[3]
            tagged = []
            for v, B in pairs:
                if B == D: side = "T" if tT == M else "F"
                elif tT in dom.get(B, ()) and tT != M: side = "T"
                elif tF in dom.get(B, ()) and tF != M: side = "F"
                else: side = "?" + str(self.lab(B, LABEL_DEPTH))
                tagged.append((side, self.H(v)))
            return ("phi", self.H(rest[0]), self.H(c), tuple(sorted(map(str, tagged))))
        tagged = [(self.H(v), self.H(B)) for v, B in pairs]
        return ("phi", self.H(rest[0]), tuple(sorted(map(str, tagged))))
    def position_vars(self):
        pos_structs = {s for s, ds in self.mdecos.items() if any("BuiltIn Position" in d for d in ds)}
        out = set()
        for rid, (op, rest) in self.defs.items():
            if op != "OpVariable": continue
            if any("BuiltIn Position" in d for d in self.decos.get(rid, [])): out.add((rid, None))
            t = rest[0]
            # pointer -> (array of)* struct
            while t in self.defs and self.defs[t][0] in ("OpTypePointer", "OpTypeArray", "OpTypeRuntimeArray"):
                tt = self.defs[t]
                t = tt[1][1] if tt[0] == "OpTypePointer" else tt[1][0]
            if t in pos_structs: out.add((rid, "member0"))
        return out
    def position_stores(self):
        pv = self.position_vars()
        res = []
        for ptr, val, blk in self.stores:
            base, idx = ptr, []
            if self.defs.get(ptr, ("",))[0] in ("OpAccessChain", "OpInBoundsAccessChain"):
                base = self.defs[ptr][1][1]; idx = self.defs[ptr][1][2:]
            for v, kind in pv:
                if base != v: continue
                if kind == "member0":
                    if not idx: continue
                    last = idx[-1]
                    c = self.defs.get(last)
                    if not (c and c[0] == "OpConstant" and c[1][-1] == "0"): continue
                res.append((ptr, val, blk))
        return res

def analyse(src, stage, defs):
    h = Hasher(parse(build(src, stage, defs)))
    out = []
    for ptr, val, blk in h.position_stores():
        out.append(h.H(val) + "@" + h.H(ptr) + ("@" + h.H(blk) if os.environ.get("POS_BLK") else ""))
    return sorted(out), len(h.cone)

if __name__ == "__main__":
    src, stage, variants = sys.argv[1], sys.argv[2], sys.argv[3:]
    ref = None
    for v in variants:
        hs, n = analyse(src, stage, v)
        tag = "REF " if ref is None else ("SAME" if hs == ref else "DIFF")
        if ref is None: ref = hs
        print(f"{tag} {src} [{v}] stores={len(hs)} cone={n} {' '.join(x[:16] for x in hs)}")
