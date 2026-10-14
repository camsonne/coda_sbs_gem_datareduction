#!/usr/bin/env bash
# gf_rc_hook.sh - CODA3 run-control external process for the gemfilter stations.
#
# Attach in jcedit (Expert -> Supervisor Process -> New...), one process per transition:
#   Name        Transition       sync  Command (Path = <prefix>/bin)
#   gf_prestart after - prestart  on   gf_rc_hook.sh prestart %(rt) %(rn) %(session)
#   gf_go       after - go        off  gf_rc_hook.sh go       %(rt) %(rn) %(session)
#   gf_end      after - end       off  gf_rc_hook.sh end      %(rt) %(rn) %(session)
#   gf_reset    before - reset    off  gf_rc_hook.sh reset    %(rt) %(rn) %(session)
#   ExitCode 0 (or 777 = ignore).  With GF_STRICT=0 (default) the hook always exits 0: if the
#   filters cannot be put in place the run continues UNFILTERED and the problem is logged.
#   With GF_STRICT=1 prestart exits non-zero and run control reports the error.
#
# The Supervisor process runs on the platform host.  The filters must run on the EB host
# (local ET = shared memory): if those differ set GF_SSH_HOST=<EB host> and the hook runs
# gemfilter_ctl there over ssh (passwordless key for the DAQ account required).
#
# Output goes to $GF_LOG_DIR/rc_hook.log (Java fork does not capture stdout well), and a
# per-run record to $GF_LOG_DIR/runs/run_<rn>.txt (config hashes, station state, counters).
set -u
GF_PREFIX=${GF_PREFIX:-$(cd "$(dirname "$0")/.." && pwd)}
# shellcheck disable=SC1091
. "$GF_PREFIX/share/gemfilter/gf_common.sh" || exit 0

tr_=${1:-}; rt=${2:-?}; rn=${3:-0}; sess=${4:-${SESSION:-?}}
mkdir -p "$GF_LOG_DIR/runs"
exec >> "$GF_LOG_DIR/rc_hook.log" 2>&1
runrec="$GF_LOG_DIR/runs/run_${rn}.txt"

ctl() {
  if [ -n "${GF_SSH_HOST:-}" ]; then
    ssh -o BatchMode=yes -o ConnectTimeout=5 "$GF_SSH_HOST" "$GF_PREFIX/bin/gemfilter_ctl" "$@"
  else
    "$GF_BIN/gemfilter_ctl" "$@"
  fi
}

gf_log "transition=$tr_ runtype=$rt run=$rn session=$sess enable=$GF_ENABLE et=$GF_ET_FILE"
if [ "$GF_ENABLE" != "1" ]; then
  gf_log "GF_ENABLE=$GF_ENABLE: gemfilter disabled, events go to the ER unfiltered"
  [ "$tr_" = "prestart" ] && echo "run=$rn gemfilter=DISABLED" > "$runrec"
  exit 0
fi

case "$tr_" in
  download)
    ctl render >/dev/null || gf_log "render failed"
    exit 0 ;;
  prestart)
    out=$(ctl ensure 2>&1); rc=$?
    echo "$out"
    {
      echo "run=$rn runtype=$rt session=$sess time=$(gf_ts) host=$(hostname)"
      echo "et_file=$GF_ET_FILE er_station=$GF_ER_STATION arms=$GF_ARMS db_dir=$GF_DB_DIR date=$GF_DB_DATE"
      for a in $GF_ARMS; do
        [ -f "$GF_RUN_DIR/$a.cfg" ] && echo "cfg[$a]: $(grep -v '^#' "$GF_RUN_DIR/$a.cfg" | tr '\n' ';')"
        "$GF_BIN/gemfilter_file" --check -p "gem:$GF_RUN_DIR/$a.cfg" 2>&1 | sed "s/^/check[$a]: /"
      done
      echo "prestart_rc=$rc"
      echo "$out" | grep -E '^(process|station|et)='
    } > "$runrec"
    if [ $rc -ne 0 ]; then
      gf_log "WARNING: gemfilter NOT in place for run $rn (rc=$rc); run continues unfiltered for the" \
             "missing arms"
      [ "$GF_STRICT" = "1" ] && exit 10
    fi
    exit 0 ;;
  go)
    ctl status | sed 's/^/go: /' | tee -a "$runrec"
    exit 0 ;;
  end)
    { echo "end_time=$(gf_ts)"; ctl status 2>&1 | sed 's/^/end: /'
      for a in $GF_ARMS; do echo "stats[$a]: $(grep -E 'events|ratio' "$(gf_logfile "$a")" 2>/dev/null | tail -1)"; done
    } | tee -a "$runrec"
    exit 0 ;;
  reset)
    if [ "$GF_STOP_ON_RESET" = "1" ]; then ctl stop; fi
    exit 0 ;;
  *)
    gf_log "unknown transition '$tr_' (download|prestart|go|end|reset)"; exit 0 ;;
esac
