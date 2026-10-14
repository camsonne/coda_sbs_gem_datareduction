# gemfilter CODA deployment kit — SBS SIDIS

This kit installs gemfilter on the SBS Event Builder host. It runs one ET station per spectrometer
arm between the EB and the ER, and drives the stations from CODA3 run control.

```
ROCs ──► EB ──► ET /tmp/<EXPID>_<EB> ──► GEMFILTER_BB (pos 1) ──► GEMFILTER_SBS (pos 2) ──► ER station ──► disk
                                          BigBite GEMs, BB-calo roads   SBS GEMs, HCal roads
```

* **Two stations in series.** A station only touches the strips its arm's APV map knows. Strips of the
  other arm are always kept, so each arm is filtered once, and the order only changes CPU use.
  A BigBite station run over SBS-arm events costs about 0.16 ms of CPU per CODA event (block of 5).
* **Never blocks the run.** If a filter dies, ET sends its events on to the ER (`restore=out`). An
  idle station, one with no attachment, is bypassed, and `GF_ENABLE=0` turns the whole thing off.
  Unfiltered events are valid raw data.
* **Each event records what was done.** Every station appends its `GEMFILTER` status bank (tag
  0x4746) to the GEM host ROC. A fully filtered event therefore carries two status banks. Word p12
  identifies the station: `0xBB0000nn` for BigBite and `0x5B5000nn` for the SBS arm, where nn is
  the station position.

## Files (after install, `<prefix>` = e.g. `/home/coda/gemfilter`)

| file | role |
|---|---|
| `bin/install_gemfilter.sh` (also `deploy/coda/` in the source tree) | configure, build, test and install |
| `etc/gemfilter_sidis.env` | **site configuration**, created once from `.env.example` and never overwritten |
| `etc/setup_gemfilter.sh`, `.csh` | source after the CODA setup to put the tools on `PATH` |
| `bin/gemfilter_check` | preflight: CODA env, ET library, names, DB, both arm configs; `--live` probes the ET |
| `bin/gemfilter_ctl` | `render`, `start`, `ensure`, `stop`, `restart`, `status`, `cmd` |
| `bin/gf_rc_hook.sh` | CODA3 run-control external process (prestart, go, end, reset) |
| `bin/gf_et_status` | ET station probe: existence, position, attachments, in/out counts |
| `bin/gemfilter_et`, `bin/gemfilter_file` | the filter (online ET mode / offline file mode, `--check`) |
| `share/gemfilter/sidis/{bb,sbs}.cfg.in` | filter configuration templates (rendered into `$GF_RUN_DIR`) |
| `share/gemfilter/build_info.txt` | build record: ET mode, library and version (read by the preflight) |
| `log/gemfilter_<arm>.log`, `log/rc_hook.log`, `log/runs/run_<N>.txt` | per-arm logs, hook log, per-run record |

## 1. Install on the EB host

```bash
source /site/coda/3.10/.setup            # or your coda_scripts setupCODA3.bash (sets CODA, EXPID, SESSION)
git clone https://github.com/camsonne/coda_sbs_gem_datareduction && cd coda_sbs_gem_datareduction
deploy/coda/install_gemfilter.sh --prefix /home/coda/gemfilter --mode coda --live-test
```

`--mode coda` links the ET and evio libraries of `$CODA`. **This is required for production.** A
local ET client maps the ET system file, so it must use the same ET version as the EB's ET system.
`--mode fetch` downloads ET and evio from GitHub, for test machines and CI. `--live-test` also runs
the live ET tests (`coda.et_live`, `deploy.kit_live`). These need a host where `et_start` may open its
ports; all of them pass in CI (ubuntu-latest, fetch mode).

## 2. Configure

Edit `<prefix>/etc/gemfilter_sidis.env`. Lines marked `[SET]` or `[VERIFY]`:

| variable | default | what to check |
|---|---|---|
| `GF_EB_NAME` | `EB1` | the EB/PEB component of the SIDIS configuration |
| `GF_ET_FILE` | `/tmp/${EXPID}_${GF_EB_NAME}` | EMU default name; compare with the EB transport `etName` in jcedit (at most 99 characters) |
| `GF_ER_STATION` | `ER1` | name of the station the ER reads from: during a run, list it with `gf_et_status -f $GF_ET_FILE --station <guess>` or look in the ER ET transport |
| `GF_DB_DIR`, `GF_DB_DATE` | SBS-replay DB, `2023-06-01` | no SIDIS DB exists yet; the date selects the GEn-II BigBite layout (DB/20220701) until it does |
| `GF_SBS_TRACKER` | `sbs.gem` | SIDIS SBS-arm tracker prefix in the DB |
| `GF_SBS_SEED` | `fadc` | `vtp` if the HCal VTP cluster bank is read out (`GF_SBS_VTP_ROC/TAG`) |
| `GF_BB_MODE`, `GF_SBS_MODE` | `road` | road mode is lossless with respect to SBS-offline; track mode only after validation |
| `GF_BB_THREADS`, `GF_SBS_THREADS` | 8, 8 | the EB shares the host; the preflight warns if threads ≥ cores |
| `GF_PRESCALE` | 1000 | every Nth physics event is written raw and flagged, to monitor the filter |
| `GF_BLOCKING`, `GF_RESTORE` | 1, `out` | blocking = no event bypasses a running filter; `out` = events of a dead filter go to the ER |
| `GF_STRICT` | 0 | 1 = prestart fails (and run control shows the error) when a filter is not in place |
| `GF_SSH_HOST` | (unset) | set it when the run-control platform host is not the EB host |

Every variable can also be overridden from the environment, for example
`GF_SBS_MODE=track gemfilter_ctl restart sbs`.

```bash
source /home/coda/gemfilter/etc/setup_gemfilter.sh
gemfilter_check            # 0 FAIL required; read every WARN
gemfilter_ctl cmd          # the exact gemfilter_et command lines
```

## 3. Wire into CODA run control (jcedit)

In jcedit, open **Expert → Supervisor Process → New…** for the SIDIS configuration. Path is
`<prefix>/bin`, and ExitCode is 0 (or 777 to ignore it).

| Name | Transition | sync | Command |
|---|---|---|---|
| `gf_prestart` | after – prestart | **on** | `gf_rc_hook.sh prestart %(rt) %(rn) %(session)` |
| `gf_go` | after – go | off | `gf_rc_hook.sh go %(rt) %(rn) %(session)` |
| `gf_end` | after – end | off | `gf_rc_hook.sh end %(rt) %(rn) %(session)` |
| `gf_reset` | before – reset | off | `gf_rc_hook.sh reset %(rt) %(rn) %(session)` |

Save the configuration, then in rcgui reset and re-select the configuration (Configurations → Coda Config).

The Prestart hook runs after every component has prestarted, so the EB's ET system and the ER
station already exist at that point. The hook:
1. renders both configs and checks that they load (`gemfilter_file --check`);
2. starts any filter that is not running. Filters keep running between runs and reconnect if the
   ET system is recreated;
3. waits up to `GF_ATTACH_WAIT_S` for both stations to be attached **upstream of the ER station**.
   A filter found at or after the ER is restarted, which re-runs `--fix-position`. This happens
   when the ER station is created after the filter at a lower position;
4. writes `log/runs/run_<N>.txt`: config hashes, the rendered configs, station positions and the
   result. The End hook adds the station in/out counts. Paste this file into the run's elog entry.

Without `GF_STRICT=1` the hook always exits 0. If a filter cannot be put in place, the run goes on
**unfiltered** for that arm and `rc_hook.log` carries a `WARNING ... NOT in place` line.

## 4. Commissioning checklist on the DAQ (in order)

1. `gemfilter_check` passes with 0 FAIL. `build_info.txt` says `et_mode=coda` with a library under `$CODA`.
2. `ctest` with `--live-test` passes on the EB host (fetch mode gives the same test with the GitHub ET).
3. Download and Prestart a pulser run with `GF_ENABLE=0`, then `gemfilter_check --live`.
   Expected: the ER station is found and both filters are missing. This confirms `GF_ET_FILE` and
   `GF_ER_STATION`.
4. With `GF_ENABLE=1` and the `GF_PRESCALE` default: run a short cosmic or pulser run.
   `gemfilter_ctl status` must show BB=1, SBS=2 and ER=3, `attachments=1`, and the in counts must
   rise during the run.
5. Run offline on that run's file: `gemfilter_file -i <run>.evio.0 --list-banks`.
   There must be two 0x4746 banks per physics block, and the prescaled raw events must be flagged.
   Then replay it with SBS-replay and check the GEM track lists against the prescaled raw events.
6. Kill one filter during a run (`kill -9 $(cat $GF_RUN_DIR/bb.pid)`). The run must continue with
   no deadtime spike, and the next Prestart restarts the filter.
7. Rate test at the SIDIS trigger rate: the filter must not throttle the EB. Watch
   `gf_et_status ... in=` and the ER rate. If it does throttle, raise the thread counts or use
   `GF_BLOCKING=0`.

## Verified so far

| check | where | result |
|---|---|---|
| `deploy.kit_static`: syntax, rendering, `--check` of both arms, preflight error paths, hook without ET, strict mode, enable switch | macOS sandbox + CI | pass |
| `deploy.kit_live` A: prestart hook → BB=1, SBS=2, ER=3; all events reach the ER; ×14.5 reduction; 2 status banks per event | CI ubuntu-latest, real `et_start` (GitHub ET 16.5) | pass (6df74e2): 63/63 events, 50.6 → 3.5 MB, 120 status banks for 60 blocks |
| `deploy.kit_live` B: ER created upstream → status rc 2 → hook restarts the filters in front | CI | pass: rc 2 detected, final BB=1 SBS=2 ER=3 |
| `deploy.kit_live` C: BB filter SIGKILL → events still reach the ER, SBS still filters, next prestart restarts BB | CI | pass: 63/63 events, 60 status banks (SBS only) |
| BB station on SBS-arm data in series (file mode) | sandbox | all foreign strips kept, +60 B and 0.16 ms CPU per CODA event |
| real CODA 3.10 EB/ER, `$CODA` ET library, jcedit Supervisor processes | **not yet**: DAQ host | — |
| SIDIS DB, SIDIS crate map / ROC ids, ER station name, ET file name | **not yet** | defaults are GMn/GEn-II based |

All reduction figures so far come from synthetic events (see `docs/bench_synthetic.csv`).
