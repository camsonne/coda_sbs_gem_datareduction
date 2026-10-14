# gemfilter — online GEM data suppression with calorimeter-seeded road tracking for SBS (CODA3)

Version 0.1 (October 2026). Target: the next SBS experiment (SIDIS: BigBite electron arm + SBS hadron arm),
with validation on raw data from GMn, GEn-II and GEp.

## 1. Goal and principle

The GEM trackers dominate the SBS data volume: 600 MB/s from the BigBite GEMs at 3 kHz in GMn, about
1 GB/s in GEp production, and GEM occupancy near 40 % limited the GEp beam current. The online zero suppression in
the MPD/VTP keeps every strip above threshold, but offline tracking only uses strips near the
calorimeter-defined track roads.

gemfilter removes those strips before the Event Recorder (ER) writes them. It is a CODA3 software filter that runs as an ET station
between the Event Builder (EB) and the ER:

```
ROCs ──► EB ──► ET system ──► [GEMFILTER station, N threads] ──► [ER station] ──► disk
                                 │  parse built event (block of N physics events)
                                 │  decode MPD banks (SSP/VTP format)
                                 │  calorimeter seeds (FADC250 clusters or VTP clusters)
                                 │  constraint points → per-module U/V windows (SBS-offline logic)
                                 │  road filter / crude tracking → keep mask
                                 │  re-encode MPD banks, shrink event in place, append status bank
```

**Key property (road mode).** With constraint points enabled, SBS-offline (`SBSGEMTrackerBase.cxx` around line 1281,
`SBSGEMModule.cxx` around line 3080) runs 1D cluster finding only inside the U/V interval spanned by the corridor between the
front and back constraint windows at each module. gemfilter computes exactly that interval
(`seeds/Roads.cxx`: same line, same linear width interpolation, same corner projection to U/V). Strips outside it
could never enter an offline track, so with the same constraint widths the online cut is lossless with respect to
offline tracking. A configurable width scale and margin add safety on top.

**Track mode (optional, tighter).** Inside the roads, gemfilter forms 2D hits, searches straight-line candidates
(at least `min_layers` layers, χ² cut), and keeps only clusters that lie along accepted candidates, plus a margin. It falls back to
road mode in three cases: no candidate, best candidate with fewer than `min_layers_confident` layers, or more than `max_hits2d_track`
2D hits in the road (to protect CPU time and efficiency at high occupancy).

**Safety.** The filter never drops an event; it only removes strips. Control, sync and user (EPICS, scaler) events pass bit-exact.
Any decode or encode error leaves the event unmodified and sets a flag. Every Nth physics event can pass raw (prescale), flagged.
Each processed event carries a GEMFILTER status bank (tag 0x4746) with the configuration hash, strips and bytes in/out,
seeds, roads, tracks and CPU time, so offline analysis can check exactly what was done.

## 2. Software layout (C++17, CMake, no ROOT dependency)

| dir | library | content | origin |
|---|---|---|---|
| `codec/` | gemfilter_codec | MPD/SSP/VTP decoder; bit-exact re-encoder (all-true mask ⇒ identical bytes); synthetic generators; `write_block` | GEM codec track |
| `evioio/` | gemfilter_evioio | CODA3 built-event parser (trigger bank, ROC banks), in-place editor, evio v4/v6 file I/O | GEM codec track |
| `geom/` | gemfilter_geom | Podd DB parser with timestamp selection and provenance; GEM, calorimeter and crate-map readers; APV→strip maps (INFN/UVA); BigBite/SBS arm configs | Seeds track |
| `seeds/` | gemfilter_seeds | FADC250 decode and pulse processing, calorimeter clustering, preshower match, VTP cluster decode, **constraint points and module windows** | Seeds track + merge |
| `track/` | gemfilter_track | strip mapping, 1D clusters, 2D hits, road tracker, keep masks; toy generator | merge (Phase 2) |
| `pipeline/` | gemfilter_pipeline | `GemFilterProcessor` (the production EventProcessor), config parser, shared context | merge |
| `status/` | gemfilter_status | event classification, policy (prescale, status bank, error handling) | CODA track |
| `et/` | gemfilter_et(_core) | ET station filter: chunked get/put, worker pool, ordered put, in-place shrink, reconnect; EVIO v4/v6 ET-buffer framing | CODA track |
| `apps/` | — | `gemfilter_file` (file mode), `gemfilter_et` (daemon), processor registry | CODA track |
| `tools/` | — | `gemfilter_bench` (scan, `--synth` writer), `gf_et_feed` / `gf_et_drain` (ET replay) | merge |

Tests (CTest, 7 suites, all passing): `codec.unit`, `evioio.unit`, `seeds.unit` (against the SBS-replay DB snapshot in
`tests/seeds/data/DB`), `track.unit`, `pipeline.e2e`, `coda.status`, `coda.et_mock`. The opt-in `coda.et_live` test
(`-DGEMFILTER_LIVE_ET_TEST=ON`) runs `et_start` → drain (ER) → `gemfilter_et` → feeder; it needs a machine where
ET may bind its ports, so it was not run in the development sandbox.

## 3. CODA integration

* **Placement.** `gemfilter_et -f <EB ET file> --position 1 --er-station <ER station name>`. The filter station is created
  blocking and upstream of the ER station. `--fix-position` moves it if the ER attached first. In CODA run control the
  ER configuration stays unchanged; the filter is started before Go (for example from the run-control start script) and attaches to the
  EB's ET system.
* **Failure mode.** If the filter process dies, ET returns its events according to `--restore` (default `out`: events
  go downstream to the ER, unfiltered). The ER therefore never stalls, and unfiltered events stay valid raw data. Use a non-blocking
  station (`--nonblocking --cue N`) if you would rather bypass the filter than slow the run when the filter falls behind.
* **Ordering.** Chunks are processed in parallel and put back in order (verified by `coda.et_mock`: 130 CODA events,
  4 threads, order preserved; reconnect after ET death without duplicates).
* **Event size.** Filtered events only shrink and are rewritten in place (`et_event_setlength`). An event that would
  grow (only the 15-word status bank is added) passes unmodified (`--oversize pass`).
* **Precedent.** This is the same placement as an L3 software trigger farm attached to the EB output ET.
* **What it does not fix.** The MPD→VTP fibre link limit (5 kHz in GEp because of the MPD noise issue) is upstream of the
  EB. gemfilter relieves the EB→ER→disk path (GEp: 2.8 GB/s pulser limit, about 1 GB/s production with 3 ERs) and the disk and
  tape volume.

## 4. Configuration (`-p gem:<file>`)

```
db_dir = /group/halla/.../SBS-replay/DB   # Podd DB tree
date   = 2026-11-01 00:00:00              # selects DB timestamp blocks (run date)
arm    = bb                               # bb | sbs
gem_tracker = bb.gem                      # DB prefix
gem_bank_tags = 3561
seed_source = fadc                        # fadc (BBCal/HCal FADC250 clustering) | vtp | none
calo = bb.sh
calo_ps = bb.ps
mode = road                               # road (lossless vs offline) | track
road_width_scale = 1.0                    # multiply DB constraint widths
road_margin = 0.0                         # [m] added to half-widths
max_seeds = 4                             # highest-energy clusters turned into roads
no_seed = keep                            # keep | drop | global
# track mode
min_layers = 3
min_layers_confident = 5
max_hits2d_track = 150
keep_margin_strips = 3
keep_track_tol = 0.004
```
The constraint widths, offsets and BigBite optics coefficients come from the same DB keys SBS-offline reads
(`frontconstraintwidth_x`, `optics_parameters`, `preconflag`, …). The online acceptance therefore follows the replay database.
The configuration hash goes into every status bank.

## 5. Performance on synthetic data

**All results in this section are SYNTHETIC.** They use the real SBS-replay geometry and APV maps, toy straight tracks, uniform random
background clusters, and an Apple M-series core with one thread. `gemfilter_bench` scans background strip occupancy from 5 % to 40 %
(Fig. `gemfilter_benchmark.png`; table `gemfilter_bench_table.csv`).

| occupancy | SBS full pipeline, road: reduction / retention / CPU per event | SBS track | BB filter only, road | BB track |
|---|---|---|---|---|
| 5 % | ×14.7 / 100 % / 0.42 ms | ×56 / 100 % / 0.45 ms | ×5.9 / 100 % / 0.08 ms | ×16 / 99.4 % |
| 10 % | ×16.0 / 100 % / 0.72 ms | ×65 / 100 % / 0.88 ms | ×6.1 / 100 % / 0.15 ms | = road (fallback) |
| 20 % | ×16.7 / 100 % / 1.27 ms | ×36 / 100 % / 2.97 ms | ×6.2 / 100 % / 0.29 ms | = road |
| 30 % | ×17.1 / 100 % / 1.75 ms | ×32 / 99.7 % / 6.2 ms | ×6.2 / 100 % / 0.41 ms | = road |
| 40 % | ×17.2 / 100 % / 2.16 ms | ×17 (fallback) / 100 % / 2.3 ms | ×6.2 / 100 % / 0.52 ms | = road |

Observations:
* Road mode is lossless at every occupancy, and its CPU cost is linear in the number of input strips (65–85 ns per strip
  for the whole chain: EVIO parse, MPD decode, mapping, clustering, windows, re-encode, rebuild).
* The BigBite production constraint windows (front 10 × 7 cm, back 6 × 7 cm half-widths in the DB snapshot) are wide.
  They give ×6 in road mode, and track mode adds nothing above 10 % occupancy because the safety guards fall back to road. Tighter
  widths are the lever on BigBite, and they must be matched in the offline replay.
* SBS-arm windows (4 cm) give ×15–17 in road mode, and track mode reaches ×30–65 up to 30 % occupancy.
* File mode on a 168 MB synthetic run (1,000 events, 15 % occupancy): 168.5 MB → 10.2 MB in road mode, 3.9 MB in track mode.
* Track mode accepts about 5.4 candidates per physics event at 15 % occupancy when there is one true track (the metrics CSV
  reports 27.2 per CODA event, and each CODA event is a block of 5 physics events with 5.0 seeds per block). The extras are background
  ghosts inside the 4 cm roads. They cost reduction, not efficiency. A tighter `max_chi2ndf` or `search_window` is the next tuning step.

**CPU budget.** Cores ≈ rate × CPU per event. Scaled to GMn-like input (600 MB/s at 3 kHz, about 200 kB GEM per event,
about 17k strips): about 1.2 ms per event, or 4 cores at 3 kHz and 12 cores at 10 kHz, before any SIMD or decode
optimisation. With 2× headroom, a single 32-core DAQ node covers 10 kHz. The real per-strip cost must be measured on DAQ hardware with
real files (Phase 3).

## 6. Validation plan with real data (Phase 3, pending files)

1. `gemfilter_file -i <run>.evio --list-banks` to confirm the ROC and bank inventory (GEM 3561 banks, FADC and VTP banks).
2. Decode-only comparison with SBS-offline: `-p passthrough` and dump strip counts per APV, compared with the replay's
   `sbs.gem*.m*.strip.*` / `bb.gem.m*.strip.*` multiplicities, for one GMn (SSP), one GEn-II (VTP) and one GEp run.
3. Filter pass in road mode with production widths, then replay the original and the filtered file with the same DB.
   Requirement: identical track lists (track count, χ², hit lists) on more than 99.9 % of events. Report reduction, CPU per event and
   `no_seed` fraction.
4. Track mode and width scans: retention of offline tracks (good-electron sample with W² and E/p cuts) against reduction.
5. Live ET test on a DAQ test machine (`tests/coda/et_live_test.sh`) at the target rate.

## 7. Open items (UNVERIFIED in code, check on real data)

* MPD event-header TRIGGER_NUMBER width (27 vs 22 bits) and placement of the type-12/13 words within frames
  (`docs/gem_format.md` §9). The codec carries unknown words verbatim, so re-encoding is safe.
* VTP cluster energy units and HCal subtype (11 vs 13), and the energy mask (14 vs 16 bits): set `vtp_energy_scale` and
  `seed_emin` after comparing with FADC clusters. FADC seeding avoids these questions.
* EVIO version written into ET by the SBS CODA EMU (v4 vs v6). Both framings are implemented and tested.
* Status-bank tag 0x4746 must not collide with any ROC bank tag of the new readout lists.
* SIDIS DB: SBS-replay has no SIDIS-specific GEM or calorimeter DB yet. The configs must point to the SIDIS geometry once it
  exists. Until then the GMn/GEn/GEp timestamps are used for validation.
* No-seed policy: the default `keep` writes all strips when no calorimeter seed is found. Measure the `no_seed` fraction on real data.
