"""Read, summarise and plot gemfilter metrics CSVs.

    python -m gfmetrics metrics_road.csv [metrics_track.csv ...] [--plot out.png]
"""
from __future__ import annotations
import sys
import pandas as pd

FLAGS = {0: "prescaled_raw", 1: "decode_error", 2: "no_seed", 3: "no_room", 4: "not_physics",
         5: "no_gem_roc", 6: "malformed", 7: "unsupported_framing", 8: "status_not_appended"}


def load(path: str) -> pd.DataFrame:
    d = pd.read_csv(path)
    d["flags_int"] = d["flags"].apply(lambda s: int(str(s), 16) if str(s).startswith("0x") else int(s)).astype("int64")
    for bit, name in FLAGS.items():
        d[name] = (d["flags_int"] // (1 << bit)) % 2
    d["source"] = path
    return d


def summary(d: pd.DataFrame) -> dict:
    p = d[d["class"] == "physics"]
    out = {
        "file": d["source"].iloc[0], "events": len(d), "physics": len(p),
        "modified": int(p["modified"].sum()),
        "gem_reduction": p["gem_bytes_in"].sum() / max(1, p["gem_bytes_out"].sum()),
        "event_reduction": p["bytes_in"].sum() / max(1, p["bytes_out"].sum()),
        "strip_keep_frac": p["strips_out"].sum() / max(1, p["strips_in"].sum()),
        "cpu_us_mean": p["cpu_us"].mean(), "cpu_us_p99": p["cpu_us"].quantile(0.99),
        "seeds_per_block": p["nseeds"].mean(), "tracks_per_block": p["ntracks"].mean(),
    }
    for name in FLAGS.values():
        out[f"frac_{name}"] = p[name].mean() if len(p) else 0.0
    return out


def compare(paths) -> pd.DataFrame:
    return pd.DataFrame([summary(load(p)) for p in paths])


def plot(paths, out: str):
    import matplotlib.pyplot as plt
    fig, axs = plt.subplots(1, 2, figsize=(7, 2.6), constrained_layout=True)
    for p in paths:
        d = load(p)
        d = d[d["class"] == "physics"]
        axs[0].hist(d["gem_bytes_in"] / d["gem_bytes_out"].clip(lower=1), bins=50, histtype="step", label=p)
        axs[1].hist(d["cpu_us"] / 1000, bins=50, histtype="step", label=p)
    axs[0].set_xlabel("GEM bytes in / out per event"); axs[1].set_xlabel("CPU per CODA event (ms)")
    for a in axs:
        a.set_ylabel("events")
    axs[0].legend(frameon=False, fontsize=6)
    fig.savefig(out, dpi=200)
    return out


def main(argv=None):
    argv = list(sys.argv[1:] if argv is None else argv)
    out = None
    if "--plot" in argv:
        i = argv.index("--plot"); out = argv[i + 1]; del argv[i:i + 2]
    t = compare(argv)
    with pd.option_context("display.width", 200, "display.max_columns", 50):
        print(t.T.to_string())
    if out:
        plot(argv, out)


if __name__ == "__main__":
    main()
