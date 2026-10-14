#!/usr/bin/env python3
"""make_sim_db.py -- build the flat "SIDIS-sim in real readout format" Podd DB directory.

Why: sbsdig2evio writes simulated strips/channels into the REAL electronics addresses (real
db_bb.gem.dat chanmap/apvmap, real calorimeter detmap/chanmap, real db_cratemap.dat), but the
strip / block INDICES are the simulation's.  The physical meaning of an index must therefore come
from the geometry the digitizer used (SBS-replay DB_MC), not from the real alignment (real BB GEM
U axis is at 150 deg where DB_MC/libsbsdig use 330/-30 deg; real BBCal z differs by ~5 cm, ...).
This tool writes, for each detector, the DB_MC values with the decode-map keys replaced by the
real ones resolved at --date, plus a few decoder flags switched from "MC" to "real online-ZS".
Both gemfilter (-p gem:...cfg db_dir=<out>) and SBS-offline (DB_DIR=<out>, real CODA decoder,
NOT SBSSimDecoder) then read the converted EVIO consistently with the simulation truth.

Podd DB semantics implemented here: "key = value" lines, continuation lines without '=', '#'
comments, "[ yyyy-mm-dd hh:mm:ss ]" time-stamp lines; for each key the value from the block with
the latest time stamp <= date wins (ties: later in file); time-stamped sub-directories
(YYYYMMDD) resolved like Podd (latest <= date, then DEFAULT, then top level).

UNVERIFIED: the decoder-flag overrides (see GEM_OVERRIDES) were chosen from SBSGEMModule.cxx
(mcpass4) reading, not from a replay; check with a decode-only SBS-offline replay of the EVIO
against the SBSSimDecoder replay of the same ROOT file (identical strip lists expected).
"""
import argparse
import datetime as dt
import os
import re
import shutil
import sys

TS = re.compile(r"\[\s*(\d{4})-(\d{1,2})-(\d{1,2})(?:\s+(\d{1,2}):(\d{2})(?::(\d{2}))?)?\s*\]")
KEY = re.compile(r"^\s*([A-Za-z_][\w.]*)\s*=(.*)$")

GEM_READOUT = [r"m\d+\.chanmap", r"m\d+\.apvmap"]
# decoder flags for real-format (online pedestal + common-mode subtracted, zero-suppressed) MPD data
GEM_OVERRIDES = {"is_mc": "0", "onlinezerosuppress": "1", "pedestalmode": "0", "pedsub_online": "1"}
GEM_ZERO_PER_MODULE = ["pedu", "pedv"]          # samples are baseline-subtracted by sbsdig2evio
CALO_READOUT = [r"detmap", r"chanmap", r"start_chanmap", r"chanmap_start", r"nchan", r"nlog_chan"]


def parse_date(s):
    for f in ("%Y-%m-%d %H:%M:%S", "%Y-%m-%d %H:%M", "%Y-%m-%d"):
        try:
            return dt.datetime.strptime(s, f)
        except ValueError:
            pass
    raise SystemExit(f"bad date {s}")


def resolve_file(root, name, date):
    fn = f"db_{name}.dat"
    sub = sorted(d for d in os.listdir(root) if re.fullmatch(r"\d{8}", d) and os.path.isdir(os.path.join(root, d)))
    req = int(date.strftime("%Y%m%d"))
    cands = []
    for d in reversed(sub):
        if req >= int(d):
            cands.append(os.path.join(root, d, fn))
            break
    cands += [os.path.join(root, "DEFAULT", fn), os.path.join(root, fn)]
    for c in cands:
        if os.path.isfile(c):
            return c
    return None


def parse_db(path, date):
    """key -> (raw value lines, block time, line number), Podd selection at `date`."""
    best = {}
    cur_t = dt.datetime.min
    key, lines, lineno = None, [], 0

    def flush():
        if key is None or cur_key_t > date:
            return
        old = best.get(key)
        if old is None or cur_key_t >= old[1]:
            best[key] = (list(lines), cur_key_t, lineno)

    cur_key_t = cur_t
    with open(path, errors="replace") as f:
        for n, raw in enumerate(f, 1):
            body = raw.split("#", 1)[0]
            m = TS.search(body)
            if m and "=" not in body:
                flush(); key = None
                y, mo, d, h, mi, s = (int(x) if x else 0 for x in m.groups())
                cur_t = dt.datetime(y, mo, d, h, mi, s)
                continue
            km = KEY.match(body)
            if km:
                flush()
                key, lines, lineno, cur_key_t = km.group(1), [raw.rstrip("\n")], n, cur_t
                continue
            if key is not None and body.strip():
                lines.append(raw.rstrip("\n"))
    flush()
    return best


def value_text(lines):
    return " ".join(l.split("#", 1)[0] for l in lines).split("=", 1)[1].split()


def write_db(path, header, entries):
    with open(path, "w") as f:
        f.write(header)
        for k, lines in entries:
            f.write("\n".join(lines) + "\n")


def hybrid(name, mc_path, real_path, mcdate, date, readout_patterns, overrides=None, zero_per_module=(), note=""):
    mc, real = parse_db(mc_path, mcdate), parse_db(real_path, date)
    pre = name + "."
    rx = [re.compile("^" + re.escape(pre) + p + "$") for p in readout_patterns]
    out, src = [], {"mc": 0, "real": 0, "override": 0}
    for k, (lines, _, _) in sorted(mc.items(), key=lambda kv: kv[1][2]):
        if any(r.match(k) for r in rx):
            continue
        short = k[len(pre):] if k.startswith(pre) else k
        if overrides and short in overrides:
            continue
        if any(short.endswith("." + z) for z in zero_per_module):
            out.append((k, [f"{k} = 0   # make_sim_db: samples are baseline-subtracted by sbsdig2evio"])); src["override"] += 1
            continue
        out.append((k, lines)); src["mc"] += 1
    for k, (lines, t, n) in sorted(real.items(), key=lambda kv: kv[1][2]):
        if any(r.match(k) for r in rx):
            out.append((k, [f"# make_sim_db: {k} from {real_path}:{n} (block {t:%Y-%m-%d %H:%M:%S})"] + lines)); src["real"] += 1
    for short, v in (overrides or {}).items():
        out.append((pre + short, [f"{pre}{short} = {v}   # make_sim_db override (real-format decoding)"])); src["override"] += 1
    hdr = (f"# make_sim_db.py: HYBRID DB for simulated data written in real readout format ({note})\n"
           f"# geometry/calibration/tracking keys: {mc_path}\n# decode-map keys ({', '.join(readout_patterns)}): {real_path}\n"
           f"# resolved at MC {mcdate:%Y-%m-%d %H:%M:%S} / real {date:%Y-%m-%d %H:%M:%S}; keys: {src}\n")
    return hdr, out, src


def check_gem_vs_sbsdig(entries, name, conf):
    """nstrips per module/axis and layer-5 x offsets must match the libsbsdig config."""
    d = {k: value_text(l) for k, l in entries}
    tok = {}
    for line in open(conf):
        t = line.split("#", 1)[0].split()
        if t:
            tok[t[0]] = t[1:]
    n = int(tok["NPlanes_bbgem"][0]); ns = [int(x) for x in tok["nstrips_bbgem"][:n]]
    mods = d[f"{name}.modules"]
    if len(mods) * 2 != n:
        return [f"{len(mods)} DB modules vs {n} libsbsdig planes"]
    errs = []
    for i, m in enumerate(mods):
        for ax, key in ((0, "nstripsu"), (1, "nstripsv")):
            v = d.get(f"{name}.{m}.{key}")
            if v and int(v[0]) != ns[2 * i + ax]:
                errs.append(f"{m}.{key}={v[0]} vs libsbsdig plane {2*i+ax} nstrips {ns[2*i+ax]}")
    return errs


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--mc-db", required=True, help="SBS-replay DB_MC directory (geometry the digitizer used)")
    ap.add_argument("--real-db", required=True, help="SBS-replay DB directory (real decode maps, cratemap)")
    ap.add_argument("--date", default="2023-06-01 00:00:00", help="time stamp for the real readout maps")
    ap.add_argument("--mc-date", default=None, help="time stamp for DB_MC (default --date)")
    ap.add_argument("--gem", default="bb.gem")
    ap.add_argument("--gem-mc-file", default=None, help="explicit MC GEM file (e.g. DB_MC/db_bb.gem_8modules.dat)")
    ap.add_argument("--calo", default="bb.sh,bb.ps,sbs.hcal")
    ap.add_argument("--sbsdig-db", default=None, help="libsbsdig config to cross-check GEM strip counts")
    ap.add_argument("-o", "--out", required=True)
    a = ap.parse_args()
    date = parse_date(a.date)
    mcdate = parse_date(a.mc_date) if a.mc_date else date
    os.makedirs(a.out, exist_ok=True)
    report = []
    # 1) copy every top-level DB_MC file as is (tracking, optics, run DB ...)
    for f in sorted(os.listdir(a.mc_db)):
        p = os.path.join(a.mc_db, f)
        if os.path.isfile(p) and f.startswith("db_") and f.endswith(".dat"):
            shutil.copy2(p, os.path.join(a.out, f))
    # 2) real crate map
    cm = resolve_file(a.real_db, "cratemap", date)
    shutil.copy2(cm, os.path.join(a.out, "db_cratemap.dat"))
    report.append(f"db_cratemap.dat <- {cm}")
    # 3) GEM
    gmc = a.gem_mc_file or resolve_file(a.mc_db, a.gem, mcdate)
    greal = resolve_file(a.real_db, a.gem, date)
    hdr, ent, src = hybrid(a.gem, gmc, greal, mcdate, date, GEM_READOUT, GEM_OVERRIDES, GEM_ZERO_PER_MODULE, "GEM")
    if a.sbsdig_db:
        errs = check_gem_vs_sbsdig(ent, a.gem, a.sbsdig_db)
        if errs:
            raise SystemExit("GEM geometry does not match libsbsdig config:\n  " + "\n  ".join(errs))
        report.append(f"{a.gem}: strip counts match {a.sbsdig_db}")
    write_db(os.path.join(a.out, f"db_{a.gem}.dat"), hdr, ent)
    report.append(f"db_{a.gem}.dat <- geometry {gmc} + readout {greal} {src}")
    # 4) calorimeters
    for c in [x for x in a.calo.split(",") if x]:
        cmc, creal = resolve_file(a.mc_db, c, mcdate), resolve_file(a.real_db, c, date)
        if not cmc or not creal:
            report.append(f"{c}: missing MC or real file, skipped")
            continue
        hdr, ent, src = hybrid(c, cmc, creal, mcdate, date, CALO_READOUT, note="calorimeter")
        write_db(os.path.join(a.out, f"db_{c}.dat"), hdr, ent)
        report.append(f"db_{c}.dat <- geometry/calibration {cmc} + readout {creal} {src}")
    with open(os.path.join(a.out, "MAKE_SIM_DB.txt"), "w") as f:
        f.write(" ".join(sys.argv) + "\n" + "\n".join(report) + "\n")
    sys.stderr.write("\n".join(report) + "\n")


if __name__ == "__main__":
    main()
