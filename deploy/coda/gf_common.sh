# gf_common.sh - shared helpers for the gemfilter CODA deployment scripts (bash >= 3.2).
# Sourced, not executed.  Expects GF_PREFIX (install prefix) to be set by the caller.
# Layout after `cmake --install`:
#   $GF_PREFIX/bin/{gemfilter_et,gemfilter_file,gf_et_status,gemfilter_ctl,gf_rc_hook.sh,...}
#   $GF_PREFIX/etc/gemfilter_sidis.env            (site configuration, edited by hand)
#   $GF_PREFIX/share/gemfilter/sidis/*.cfg.in      (filter config templates)
#   $GF_PREFIX/share/gemfilter/gf_common.sh        (this file)

: "${GF_PREFIX:?GF_PREFIX must be set}"
GF_BIN=${GF_BIN:-$GF_PREFIX/bin}
GF_TEMPLATES=${GF_TEMPLATES:-$GF_PREFIX/share/gemfilter/sidis}
GF_ENV_FILE=${GF_ENV_FILE:-$GF_PREFIX/etc/gemfilter_sidis.env}

if [ -f "$GF_ENV_FILE" ]; then
  # shellcheck disable=SC1090
  . "$GF_ENV_FILE"
elif [ -f "$GF_PREFIX/etc/gemfilter_sidis.env.example" ]; then
  echo "gemfilter: WARNING: $GF_ENV_FILE missing, using the example defaults" >&2
  # shellcheck disable=SC1090
  . "$GF_PREFIX/etc/gemfilter_sidis.env.example"
else
  echo "gemfilter: ERROR: no site configuration ($GF_ENV_FILE)" >&2
  return 1 2>/dev/null || exit 1
fi
GF_LOG_DIR=${GF_LOG_DIR:-$GF_PREFIX/log}

gf_ts() { date '+%Y-%m-%d %H:%M:%S'; }
gf_log() { echo "$(gf_ts) [$(basename "$0")] $*"; }
gf_upper() { echo "$1" | tr '[:lower:]' '[:upper:]'; }

gf_station() { echo "${GF_STATION_PREFIX}$(gf_upper "$1")"; }
gf_threads() { case "$1" in bb) echo "$GF_BB_THREADS";; sbs) echo "$GF_SBS_THREADS";; *) echo 4;; esac; }
# position of arm k (1-based) in GF_ARMS = ET station position (GrandCentral is 0)
gf_position() {
  local i=1 a
  for a in $GF_ARMS; do [ "$a" = "$1" ] && { echo $i; return; }; i=$((i + 1)); done
  echo 1
}
# status-bank word p12: 0xBB0000nn for BigBite, 0x5B5000nn for the SBS arm (nn = position)
gf_instance_id() {
  case "$1" in
    bb)  printf '0x%08x' $((0xBB000000 + $(gf_position bb))) ;;
    sbs) printf '0x%08x' $((0x5B500000 + $(gf_position sbs))) ;;
    *)   printf '0x%08x' $((0x6F000000 + $(gf_position "$1"))) ;;
  esac
}
gf_cfg() { echo "$GF_RUN_DIR/$1.cfg"; }
gf_pidfile() { echo "$GF_RUN_DIR/$1.pid"; }
gf_logfile() { echo "$GF_LOG_DIR/gemfilter_$1.log"; }

# ET connection arguments shared by gemfilter_et and gf_et_status
gf_et_args() {
  local a="-f $GF_ET_FILE"
  [ -n "$GF_ET_HOST" ] && a="$a --host $GF_ET_HOST"
  [ -n "$GF_ET_PORT" ] && a="$a --port $GF_ET_PORT"
  [ -n "$GF_ET_UDP_PORT" ] && a="$a --udp-port $GF_ET_UDP_PORT"
  echo "$a"
}

# Render <arm>.cfg from the template; prints the path.  Values with '|' are rejected.
gf_render() {
  local arm=$1 tpl="$GF_TEMPLATES/$1.cfg.in" out
  out=$(gf_cfg "$arm")
  [ -f "$tpl" ] || { echo "gemfilter: no template $tpl" >&2; return 1; }
  mkdir -p "$GF_RUN_DIR" || return 1
  local v
  for v in GF_DB_DIR GF_DB_DATE GF_GEM_BANKS GF_NO_SEED GF_BB_MODE GF_BB_ROAD_SCALE GF_SBS_TRACKER \
           GF_SBS_SEED GF_SBS_VTP_ROC GF_SBS_VTP_TAG GF_SBS_MODE GF_SBS_ROAD_SCALE; do
    case "$(eval echo "\"\$$v\"")" in *'|'*) echo "gemfilter: $v contains '|'" >&2; return 1;; esac
  done
  sed -e "s|@GF_DB_DIR@|$GF_DB_DIR|" -e "s|@GF_DB_DATE@|$GF_DB_DATE|" \
      -e "s|@GF_GEM_BANKS@|$GF_GEM_BANKS|" -e "s|@GF_NO_SEED@|$GF_NO_SEED|" \
      -e "s|@GF_BB_MODE@|$GF_BB_MODE|" -e "s|@GF_BB_ROAD_SCALE@|$GF_BB_ROAD_SCALE|" \
      -e "s|@GF_SBS_TRACKER@|$GF_SBS_TRACKER|" -e "s|@GF_SBS_SEED@|$GF_SBS_SEED|" \
      -e "s|@GF_SBS_VTP_ROC@|$GF_SBS_VTP_ROC|" -e "s|@GF_SBS_VTP_TAG@|$GF_SBS_VTP_TAG|" \
      -e "s|@GF_SBS_MODE@|$GF_SBS_MODE|" -e "s|@GF_SBS_ROAD_SCALE@|$GF_SBS_ROAD_SCALE|" \
      "$tpl" > "$out.tmp" && mv "$out.tmp" "$out" || return 1
  if grep -q '@GF_[A-Z_]*@' "$out"; then
    echo "gemfilter: unrendered fields left in $out" >&2; return 1
  fi
  echo "$out"
}

# pid of the running filter for <arm>, empty if none.  Guards against pid reuse by checking
# the command name (/proc on Linux, ps elsewhere); if neither is usable, kill -0 alone decides.
gf_pid() {
  local pf p c
  pf=$(gf_pidfile "$1")
  [ -f "$pf" ] || return 0
  p=$(cat "$pf" 2>/dev/null)
  [ -n "$p" ] && kill -0 "$p" 2>/dev/null || return 0
  if [ -r "/proc/$p/cmdline" ]; then
    c=$(tr '\0' ' ' < "/proc/$p/cmdline")
  elif ps -p $$ >/dev/null 2>&1; then
    c=$(ps -p "$p" -o command= 2>/dev/null)
  else
    echo "$p"; return 0
  fi
  case "$c" in *gemfilter_et*) echo "$p";; esac
}

# gemfilter_et command line for <arm>
gf_cmdline() {
  local arm=$1 st cfg blk=""
  st=$(gf_station "$arm"); cfg=$(gf_cfg "$arm")
  [ "$GF_BLOCKING" = "1" ] || blk="--nonblocking --cue $GF_CUE"
  echo "$GF_BIN/gemfilter_et $(gf_et_args) --station $st --position $(gf_position "$arm")" \
       "--er-station $GF_ER_STATION --fix-position -t $(gf_threads "$arm") -p gem:$cfg" \
       "--prescale $GF_PRESCALE --restore $GF_RESTORE --stats $GF_STATS_S" \
       "--instance-id $(gf_instance_id "$arm") $blk $GF_EXTRA_ARGS"
}

gf_all_stations() {
  local a s=""
  for a in $GF_ARMS; do s="$s${s:+,}$(gf_station "$a")"; done
  echo "$s"
}
