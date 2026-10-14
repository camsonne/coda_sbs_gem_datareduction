#!/usr/bin/env python3
"""make_synthetic_sbsdig_tree.py -- SYNTHETIC test tree with the libsbsdig mcpass4 branch layout.

NOT SIMULATION OUTPUT. Random GEM strips / FADC waveforms laid out exactly like the dighit
branches libsbsdig mcpass4 appends to the g4sbs tree "T" (names, per-sample entry structure,
samp/tdc sentinels -1000000, common-mode baseline in GEM samples, FADC pedestal in PMT samples),
plus a few g4sbs-like Earm.BBGEM.hit entries (one straight "signal" track) so that both truth
modes of sbsdig_extract.py can be exercised.

Storage difference (documented, harmless for uproot readers): libsbsdig writes std::vector<int>
branches; uproot writes counted C arrays (Int_t[n] with a counter branch). uproot returns both as
the same jagged arrays.  Counter branches are named exactly like libsbsdig's (e.g.
Earm.BBGEM.dighit.nstrips, which in libsbsdig counts per-SAMPLE entries, not strips).
"""
import argparse
import sys

import numpy as np

try:
    import uproot
    import awkward as ak
except ImportError:  # pragma: no cover
    sys.stderr.write("make_synthetic_sbsdig_tree.py: needs uproot + awkward\n")
    sys.exit(77)

# libsbsdig mcpass4 db_gen_conf_8gemmodules_GEN2.dat BB GEM planes (16 = 8 modules x 2 axes)
GEN_NSTRIPS = [3840] * 8 + [1280, 1536] * 4
GEN_LAYER = [1, 1, 2, 2, 3, 3, 4, 4, 5, 5, 5, 5, 5, 5, 5, 5]
GEN_OFFSET = [0.0] * 8 + [-0.768, 0.0, -0.256, 0.0, 0.256, 0.0, 0.768, 0.0]
GEN_ROANGLE = [-30.0, 30.0] * 4 + [0.0, 90.0] * 4
GEN_Z = [0.0, 0.137, 0.274, 0.411, 1.5517]
NCHAN = {"Earm.BBSH.dighit": 189, "Earm.BBPS.dighit": 52, "Harm.HCal.dighit": 288}
PITCH = 4.0e-4


def track_strips(x0, y0, tx, ty):
    """Signal 'hits' and the strips libsbsdig geometry assigns to them (centre strip)."""
    hits, strips = [], set()
    for lay in range(1, 6):
        z = GEN_Z[lay - 1]
        x, y = x0 + tx * z, y0 + ty * z
        for mod in range(8):
            p0 = 2 * mod
            dx = GEN_NSTRIPS[p0] * PITCH
            if GEN_LAYER[p0] == lay and GEN_OFFSET[p0] - dx / 2 <= x <= GEN_OFFSET[p0] + dx / 2:
                break
        else:
            continue
        hits.append((lay, x, y, x + 1e-4, y + 1e-4, 2e-6))
        lx, ly = x - GEN_OFFSET[2 * mod], y - GEN_OFFSET[2 * mod + 1]
        for ipl in (0, 1):
            p = 2 * mod + ipl
            r = -np.deg2rad(GEN_ROANGLE[p])
            u = lx * np.cos(r) - ly * np.sin(r)
            s = int(np.floor((u + GEN_NSTRIPS[p] * PITCH / 2) / PITCH))
            if 0 <= s < GEN_NSTRIPS[p]:
                strips.add((p, s))
    return hits, strips


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("-o", "--output", required=True)
    ap.add_argument("-n", "--events", type=int, default=23)
    ap.add_argument("--occupancy", type=float, default=0.01, help="fraction of strips per plane with a hit")
    ap.add_argument("--with-sig", action="store_true", help="also write Earm.BBGEM.dighit.sig (patched libsbsdig)")
    ap.add_argument("--seed", type=int, default=7)
    a = ap.parse_args()
    rng = np.random.default_rng(a.seed)
    gem = {k: [] for k in ("module", "strip", "adc", "samp", "sig")}
    pmt = {pre: {k: [] for k in ("chan", "adc", "samp", "tdc")} for pre in NCHAN}
    hit = {k: [] for k in ("plane", "xin", "yin", "zin", "xout", "yout", "zout", "edep")}
    tshape = np.array([0.0, 0.35, 0.9, 1.0, 0.75, 0.45])
    for ev in range(a.events):
        hits, sigstrips = track_strips(rng.uniform(-0.5, 0.5), rng.uniform(-0.15, 0.15), rng.uniform(-0.1, 0.1), rng.uniform(-0.05, 0.05))
        strips = {}
        for p, s in sigstrips:
            for d in (-1, 0, 1):
                if 0 <= s + d < GEN_NSTRIPS[p]:
                    strips[(p, s + d)] = 1
        for p, ns in enumerate(GEN_NSTRIPS):
            for s in rng.choice(ns, size=max(1, int(a.occupancy * ns)), replace=False):
                strips.setdefault((p, int(s)), 0)
        if ev == 0:  # saturated and negative-going samples exercise the 13-bit clamp
            strips[(0, 0)] = 0
            strips[(15, GEN_NSTRIPS[15] - 1)] = 0
        e = {k: [] for k in gem}
        for (p, s) in sorted(strips):
            amp = rng.uniform(300, 2500)
            adc = np.rint(1500 + amp * tshape + rng.normal(0, 20, 6)).astype(int)
            if ev == 0 and (p, s) == (0, 0):
                adc[:] = [4096, 4096, 4096, 3000, 1500, 0]
            if ev == 0 and (p, s) == (15, GEN_NSTRIPS[15] - 1):
                adc[:] = [-3000, 1400, 1600, 1800, 1700, 1600]
            for k in range(6):
                e["module"].append(p); e["strip"].append(s); e["adc"].append(int(adc[k])); e["samp"].append(k)
                e["sig"].append(strips[(p, s)])
        for k in gem:
            gem[k].append(e[k])
        for pre, nch in NCHAN.items():
            d = {k: [] for k in ("chan", "adc", "samp", "tdc")}
            for c in sorted(rng.choice(nch, size=int(rng.integers(1, 12)), replace=False)):
                ped, amp, t0 = rng.uniform(150, 350), rng.uniform(20, 3500), rng.uniform(4, 10)
                for i in range(20):
                    v = ped + amp * max(0.0, (i - t0) / 2.0) * np.exp(1 - max(0.0, (i - t0) / 2.0)) * (i > t0)
                    d["chan"].append(int(c)); d["adc"].append(int(min(4095, np.rint(v + rng.normal(0, 2))))); d["samp"].append(i)
                    d["tdc"].append(-1000000)
                if pre == "Harm.HCal.dighit":  # TDC entries as SBSDigPMTSignal writes them
                    for tv in (1000 + int(rng.integers(0, 200)),):
                        d["chan"].append(int(c)); d["adc"].append(-1000000); d["samp"].append(-1000000); d["tdc"].append(tv)
            for k in d:
                pmt[pre][k].append(d[k])
        h = {k: [] for k in hit}
        for (lay, x, y, xo, yo, ed) in hits:
            h["plane"].append(lay); h["xin"].append(x); h["yin"].append(y); h["zin"].append(-0.001)
            h["xout"].append(xo); h["yout"].append(yo); h["zout"].append(0.001); h["edep"].append(ed)
        for k in hit:
            hit[k].append(h[k])

    def J(v, dt):
        return ak.values_astype(ak.Array(v), dt)
    with uproot.recreate(a.output) as f:
        branches, data = {}, {}
        groups = [("Earm.BBGEM.dighit", "nstrips", {k: J(gem[k], np.int32) for k in gem if k != "sig" or a.with_sig})]
        for pre in NCHAN:
            groups.append((pre, "nchan", {k: J(pmt[pre][k], np.int32) for k in pmt[pre]}))
        groups.append(("Earm.BBGEM.hit", "nhits", {k: J(hit[k], np.int32 if k == "plane" else np.float64) for k in hit}))
        for pre, cname, cols in groups:
            data[pre] = ak.zip(cols)
            branches[pre] = data[pre].type.content
        counter = {pre: f"{pre}.{c}" for pre, c, _ in groups}
        t = f.mktree("T", branches, field_name=lambda outer, inner: f"{outer}.{inner}", counter_name=lambda counted: counter[counted])
        t.extend(data)
    sys.stderr.write(f"make_synthetic_sbsdig_tree: SYNTHETIC tree with {a.events} events -> {a.output}\n")


if __name__ == "__main__":
    main()
