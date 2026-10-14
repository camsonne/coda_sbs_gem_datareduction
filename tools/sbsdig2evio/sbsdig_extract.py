#!/usr/bin/env python3
"""sbsdig_extract.py -- stage 1 of sbsdig2evio: libsbsdig (mcpass4) digitized ROOT tree -> .sdg

Reads, with uproot (no ROOT installation needed), the branches that libsbsdig adds to the g4sbs
tree "T" (g4sbs_tree.cxx / g4sbs_data.cxx, mcpass4):

  Earm.BBGEM.dighit.{nstrips,module,strip,adc,samp}   one entry per (strip, sample);
      module = libsbsdig GEM plane index p = 2*module + axis (axis 0 = X/U, 1 = Y/V),
      strip  = strip index on that plane, samp = 0..5, adc = sample incl. common-mode baseline
  Earm.BBSH.dighit / Earm.BBPS.dighit / Harm.HCal.dighit .{nchan,chan,adc,samp,tdc}
      one entry per FADC sample (samp >= 0, tdc = -1e6) or per TDC hit (samp = adc = -1e6)

and writes the .sdg intermediate file read by the C++ writer (format: sdg_format.h).

GEM truth (per strip, -1 unknown / 0 background / 1 signal), --truth:
  sig   : branch Earm.BBGEM.dighit.sig (per entry, nonzero = a signal-source hit contributed);
          NOT written by stock libsbsdig mcpass4 -- see libsbsdig_gem_sigflag.patch
  hits  : g4sbs Earm.BBGEM.hit.{plane,xin,yin,xout,yout,edep} of the signal event projected onto
          the libsbsdig strip planes with the libsbsdig geometry (--sbsdig-db), exactly as
          SBSDigAuxi::UnfoldData + SBSDigGEMSimDig::AvaModel_2 compute strip ranges, widened by
          --hit-margin strips. APPROXIMATE: a background strip inside that window is called signal.
  auto  : sig if the branch exists, else hits if --sbsdig-db given, else none.
"""
import argparse
import math
import struct
import sys

import numpy as np

try:
    import uproot
    import awkward as ak
except ImportError:  # pragma: no cover
    sys.stderr.write("sbsdig_extract.py: needs uproot + awkward (pip install uproot awkward)\n")
    sys.exit(77)

MAGIC, EVNT, END = 0x31474453, 0x544E5645, 0x21444E45
PMT_DETS = {"bbsh": (0, "Earm.BBSH.dighit"), "bbps": (1, "Earm.BBPS.dighit"), "hcal": (2, "Harm.HCal.dighit")}
GEM = "Earm.BBGEM.dighit"
GEMHIT = "Earm.BBGEM.hit"
PITCH = 4.0e-4  # libsbsdig fStripPitch [m]


def read_sbsdig_conf(path):
    """Parse the BB GEM keys of a libsbsdig db_*_conf.dat file ('key v1 v2 ...')."""
    conf = {}
    with open(path) as f:
        for line in f:
            line = line.split("#", 1)[0].strip()
            if not line:
                continue
            tok = line.split()
            conf[tok[0]] = tok[1:]
    need = ["NPlanes_bbgem", "layer_bbgem", "nstrips_bbgem", "offset_bbgem", "RO_angle_bbgem"]
    for k in need:
        if k not in conf:
            raise SystemExit(f"{path}: missing key {k}")
    n = int(conf["NPlanes_bbgem"][0])
    g = {
        "nplanes": n,
        "layer": [int(v) for v in conf["layer_bbgem"][:n]],
        "nstrips": [int(v) for v in conf["nstrips_bbgem"][:n]],
        "offset": [float(v) for v in conf["offset_bbgem"][:n]],
        "roangle": [float(v) * math.pi / 180.0 for v in conf["RO_angle_bbgem"][:n]],
    }
    g["dx"] = [ns * PITCH for ns in g["nstrips"]]
    return g


def hit_truth_strips(geo, plane, xin, yin, xout, yout, edep, margin):
    """Set of (plane, strip) touched by signal-event g4sbs GEM hits (libsbsdig geometry)."""
    out = set()
    nmod = geo["nplanes"] // 2
    for k in range(len(plane)):
        if not edep[k] > 0:
            continue
        mod = 0
        while mod < nmod:
            p0 = 2 * mod
            lo, hi = geo["offset"][p0] - 0.5 * geo["dx"][p0], geo["offset"][p0] + 0.5 * geo["dx"][p0]
            if lo <= xin[k] <= hi and plane[k] == geo["layer"][p0]:
                break
            mod += 1
        if mod == nmod:
            continue
        xo, yo = geo["offset"][2 * mod], geo["offset"][2 * mod + 1]
        x0, y0, x1, y1 = xin[k] - xo, yin[k] - yo, xout[k] - xo, yout[k] - yo
        for ipl in (0, 1):
            p = 2 * mod + ipl
            r = -geo["roangle"][p]
            c, s = math.cos(r), math.sin(r)
            u0, u1 = x0 * c - y0 * s, x1 * c - y1 * s
            s0 = int(math.floor((min(u0, u1) + 0.5 * geo["dx"][p]) / PITCH)) - margin
            s1 = int(math.floor((max(u0, u1) + 0.5 * geo["dx"][p]) / PITCH)) + margin
            s0, s1 = max(0, s0), min(geo["nstrips"][p] - 1, s1)
            for st in range(s0, s1 + 1):
                out.add((p, st))
    return out


def gem_records(mod, strip, adc, samp, sig, truth_set, truth_mode):
    """Group per-sample entries into strips; returns (n, bytes)."""
    sel = samp >= 0
    mod, strip, adc, samp = mod[sel], strip[sel], adc[sel], samp[sel]
    if sig is not None:
        sig = sig[sel]
    if mod.size == 0:
        return 0, b""
    key = mod.astype(np.int64) * 65536 + strip.astype(np.int64)
    ukey, inv = np.unique(key, return_inverse=True)
    nsamp = int(samp.max()) + 1
    samples = np.zeros((ukey.size, nsamp), dtype=np.int16)
    samples[inv, samp] = np.clip(adc, -32768, 32767).astype(np.int16)
    planes = (ukey // 65536).astype(np.uint16)
    strips = (ukey % 65536).astype(np.uint16)
    if truth_mode == "sig":
        t = np.zeros(ukey.size, dtype=np.int8)
        np.maximum.at(t, inv, (sig != 0).astype(np.int8))
    elif truth_mode == "hits":
        t = np.fromiter(((int(p), int(s)) in truth_set for p, s in zip(planes, strips)), dtype=np.int8, count=ukey.size)
    else:
        t = np.full(ukey.size, -1, dtype=np.int8)
    rec = np.zeros(ukey.size, dtype=np.dtype([("plane", "<u2"), ("strip", "<u2"), ("nsamp", "u1"), ("truth", "i1"),
                                              ("res", "<u2"), ("s", "<i2", (nsamp,))]))
    rec["plane"], rec["strip"], rec["nsamp"], rec["truth"], rec["s"] = planes, strips, nsamp, t, samples
    return ukey.size, rec.tobytes()


def pmt_records(det, chan, adc, samp, tdc):
    if chan.size == 0:
        return 0, b""
    out = []
    for c in np.unique(chan):
        m = chan == c
        sm, am, tm = samp[m], adc[m], tdc[m]
        sel = sm >= 0
        order = np.argsort(sm[sel], kind="stable")
        s = np.clip(am[sel][order], 0, 65535).astype("<u2")
        t = tm[(~sel) & (tm != -1000000)].astype("<i4")
        out.append(struct.pack("<BBHHH", det, 0, int(c), s.size, t.size) + s.tobytes() + t.tobytes())
    return len(out), b"".join(out)


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("-i", "--input", required=True, nargs="+", help="digitized ROOT file(s)")
    ap.add_argument("-o", "--output", required=True, help="output .sdg")
    ap.add_argument("-t", "--tree", default="T")
    ap.add_argument("--truth", choices=["auto", "sig", "hits", "none"], default="auto")
    ap.add_argument("--sbsdig-db", help="libsbsdig config (e.g. db_gen_conf_8gemmodules_GEN2.dat) for --truth hits")
    ap.add_argument("--hit-margin", type=int, default=4, help="strips added on each side of a hit's strip range")
    ap.add_argument("--dets", default="bbgem,bbsh,bbps,hcal", help="subset of bbgem,bbsh,bbps,hcal")
    ap.add_argument("--max-events", type=int, default=-1)
    ap.add_argument("--step-size", default="50 MB")
    a = ap.parse_args()

    dets = set(a.dets.split(","))
    files = [f"{p}:{a.tree}" for p in a.input]
    first = uproot.open(files[0])
    avail = set(first.keys())
    branches = []
    if "bbgem" in dets:
        for b in ("module", "strip", "adc", "samp"):
            if f"{GEM}.{b}" not in avail:
                raise SystemExit(f"missing branch {GEM}.{b}")
            branches.append(f"{GEM}.{b}")
    pmt_use = []
    for name, (det, pre) in PMT_DETS.items():
        if name in dets and f"{pre}.chan" in avail:
            pmt_use.append((det, pre))
            branches += [f"{pre}.{b}" for b in ("chan", "adc", "samp", "tdc")]
        elif name in dets:
            sys.stderr.write(f"sbsdig_extract: {pre} not in tree, skipped\n")
    truth = a.truth
    has_sig = f"{GEM}.sig" in avail
    if truth == "auto":
        truth = "sig" if has_sig else ("hits" if a.sbsdig_db else "none")
    if truth == "sig" and not has_sig:
        raise SystemExit(f"--truth sig: branch {GEM}.sig not in tree")
    geo = None
    if truth == "hits":
        if not a.sbsdig_db:
            raise SystemExit("--truth hits needs --sbsdig-db")
        geo = read_sbsdig_conf(a.sbsdig_db)
        for b in ("plane", "xin", "yin", "xout", "yout", "edep"):
            branches.append(f"{GEMHIT}.{b}")
    if truth == "sig":
        branches.append(f"{GEM}.sig")
    if "bbgem" not in dets:
        truth = "none"
    flags = (1 if truth == "sig" else 0) | (2 if truth == "hits" else 0)

    nev = 0
    stats = {"strips": 0, "sig": 0, "pmt": 0}
    with open(a.output, "wb") as out:
        out.write(struct.pack("<IIII", MAGIC, 1, flags, 0))
        entry0 = 0
        done = False
        for arr in uproot.iterate(files, branches, step_size=a.step_size, library="ak"):
            n = len(arr)
            cols = {}
            for b in branches:
                cols[b] = (ak.to_numpy(ak.flatten(arr[b])), np.concatenate([[0], np.cumsum(ak.to_numpy(ak.num(arr[b])))]))
            for i in range(n):
                if a.max_events >= 0 and nev >= a.max_events:
                    done = True
                    break
                def sl(b):
                    v, o = cols[b]
                    return v[o[i]:o[i + 1]]
                ng, gb = 0, b""
                if "bbgem" in dets:
                    ts = None
                    if truth == "hits":
                        ts = hit_truth_strips(geo, sl(f"{GEMHIT}.plane"), sl(f"{GEMHIT}.xin"), sl(f"{GEMHIT}.yin"),
                                              sl(f"{GEMHIT}.xout"), sl(f"{GEMHIT}.yout"), sl(f"{GEMHIT}.edep"), a.hit_margin)
                    ng, gb = gem_records(sl(f"{GEM}.module"), sl(f"{GEM}.strip"), sl(f"{GEM}.adc"), sl(f"{GEM}.samp"),
                                         sl(f"{GEM}.sig") if truth == "sig" else None, ts, truth)
                    if ng:
                        stats["sig"] += int(np.count_nonzero(np.frombuffer(gb, dtype=np.uint8).reshape(ng, -1)[:, 5] == 1))
                npm, pb = 0, []
                for det, pre in pmt_use:
                    k, bts = pmt_records(det, sl(f"{pre}.chan"), sl(f"{pre}.adc"), sl(f"{pre}.samp"), sl(f"{pre}.tdc"))
                    npm += k
                    pb.append(bts)
                out.write(struct.pack("<IIII", EVNT, entry0 + i, ng, npm))
                out.write(gb)
                out.write(b"".join(pb))
                stats["strips"] += ng
                stats["pmt"] += npm
                nev += 1
            entry0 += n
            if done:
                break
        out.write(struct.pack("<II", END, nev))
    sys.stderr.write(f"sbsdig_extract: {nev} events, {stats['strips']} GEM strips ({stats['sig']} signal, truth={truth}), "
                     f"{stats['pmt']} PMT channels -> {a.output}\n")


if __name__ == "__main__":
    main()
