# coda_sbs_gem_datareduction

**gemfilter** — a CODA3 software event filter that reduces SBS GEM (APV25/MPD) data before it is written to tape.

It runs as an ET station between the Event Builder and the Event Recorder. For each built event it decodes the MPD banks
(SSP- or VTP-era format), builds calorimeter-seeded track roads (FADC250 or VTP clusters → SBS-offline constraint
windows), removes the GEM strips outside the roads (optionally outside crude tracks), re-encodes the MPD banks
bit-exactly in the original format and appends a `GEMFILTER` status bank. The same processor runs offline on raw
EVIO files.

```
ROCs ──► EB ──► ET system ──► [GEMFILTER station, N threads] ──► [ER station] ──► disk/tape
```

Design principles:
* **Lossless w.r.t. offline tracking in road mode**: SBS-offline only clusters strips inside the U/V interval of the
  constraint corridor; gemfilter cuts at exactly that interval (same constraint widths, same interpolation).
* **Never drops an event**: on any decode/encode error the event passes through unmodified with a status flag;
  a prescaled fraction is written raw for monitoring.
* **ER unchanged**: the filter is an extra ET station upstream of the ER station; if it is not running, events flow
  through unfiltered.
* **Podd-compatible output**: the status bank (tag `0x4746`) is the last child of the GEM ROC bank, so
  `FindRocsCoda3` still accepts the event.

## Status (v0.1, October 2026)

| item | status |
|---|---|
| Build + unit/integration tests (CTest, 8 suites) | pass on macOS (Apple clang 17); Linux via CI |
| Strip reduction / CPU on **synthetic** events ([docs/bench_synthetic.csv](docs/bench_synthetic.csv), 5–40 % occupancy) | road mode: ×15.7–17.4 SBS arm, ×5.9–6.2 BigBite, signal retention 1.000, CPU per input strip 64–85 ns SBS arm (`sbs_pipeline`, full chain) and 39–42 ns BigBite (`bb_filter`); track mode: SBS arm ×76–78 at 5–10 %, ×33–37 at 20–30 %, falls back to road at 40 %; BigBite gains only at 5 % (×16, retention 0.994) |
| Raw GMn / GEn-II / GEp EVIO | **not yet validated** |
| Live ET chain (EB → GEMFILTER → ER) | `coda.et_live` and `deploy.kit_live` run in CI (`deploy-kit` job, real `et_start`, ET from GitHub); not yet on a CODA 3.10 EB |
| CODA deployment kit for SIDIS (two stations BB + SBS, run-control hook, preflight) | [deploy/coda/README.md](deploy/coda/README.md); not yet commissioned on the DAQ |

Open items to check on real data (MPD trigger-number width, VTP cluster energy units, EVIO v4/v6 framing in ET,
status-bank tag collisions, SIDIS DB, no-seed fraction) are listed in [docs/DESIGN.md §7](docs/DESIGN.md).

## Deploying on the SBS DAQ (SIDIS)
```
source $CODA/.setup
deploy/coda/install_gemfilter.sh --prefix /home/coda/gemfilter --mode coda --live-test
source /home/coda/gemfilter/etc/setup_gemfilter.sh && gemfilter_check
```
Then edit `etc/gemfilter_sidis.env` and attach `gf_rc_hook.sh` to the Prestart, Go, End and Reset
transitions in jcedit. The full guide and commissioning checklist are in
[deploy/coda/README.md](deploy/coda/README.md).

## Documentation
* [docs/DESIGN.md](docs/DESIGN.md) — algorithm, CODA integration, configuration, synthetic performance, validation plan, open items
* [docs/gem_format.md](docs/gem_format.md) — MPD/SSP/VTP GEM data format as decoded/re-encoded
* [docs/INTERFACES.md](docs/INTERFACES.md) — module layout, shared types and per-event pipeline contract
* [docs/sbsdig_mapping.md](docs/sbsdig_mapping.md) — libsbsdig → EVIO converter mapping and truth definitions

## Build
```
cmake -S . -B build -G Ninja -DGEMFILTER_ET_SOURCE_DIR=/path/et -DGEMFILTER_EVIO_SOURCE_DIR=/path/evio
#   or -DGEMFILTER_USE_CODA=ON on a CODA3 machine ($CODA), or let CMake download et/evio (default)
cmake --build build && ctest --test-dir build --output-on-failure
```
Requirements: C++17 compiler (gcc >= 9 / clang), CMake >= 3.16. No ROOT. Tested on macOS (Apple clang 17).

## Run
File mode (offline, e.g. on old runs):
```
build/bin/gemfilter_file -i e1209019_1234.evio.0.0 --list-banks            # inventory
build/bin/gemfilter_file -i in.evio -o out.evio --csv metrics.csv -p gem:config/sidis_bb_road.cfg
```
ET mode (online, start before Go; ER unchanged):
```
build/bin/gemfilter_et -f /tmp/et_sys_<expid> -p gem:config/sidis_bb_road.cfg --er-station <ER station> \
    --position 1 -t 8 --prescale 1000
```
Replay a file through a test ET system: `tests/coda/et_live_test.sh build <DB dir>`.
Synthetic data and benchmark: `build/tools/gemfilter_bench --synth <DB> synth.evio 200 0.15`,
`build/tools/gemfilter_bench <DB> 40 bench.csv` (reference output: [docs/bench_synthetic.csv](docs/bench_synthetic.csv))..

## Simulated data (libsbsdig → EVIO)
`tools/sbsdig2evio/`: `sbsdig_extract.py` (uproot: digitized ROOT tree → `.sdg`), `make_sim_db.py` (hybrid DB: DB_MC geometry +
real decode maps), `sbsdig2evio` (`.sdg` → CODA3 EVIO with real MPD 3561 / FADC250 banks + GEM truth sidecar),
`sbsdig2evio_verify` (round-trip check). Mapping and truth definitions: [docs/sbsdig_mapping.md](docs/sbsdig_mapping.md).
Config for converted files: `config/sidis_bb_sim_road.cfg`.
