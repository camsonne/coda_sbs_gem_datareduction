#!/usr/bin/env bash
# deploy.kit_live: the CODA deployment kit against a real ET system (needs a host where
# et_start may bind its TCP/UDP ports: DAQ test machine or CI runner).
#   kit_live_test.sh <build dir> <source dir> <test DB dir> [nblocks=60]
# et_start stands in for the EB's ET system, gf_et_drain for the ER, gf_et_feed for the EB.
#   A  normal cycle: ER attached first, prestart hook starts GEMFILTER_BB (1) and GEMFILTER_SBS (2)
#      upstream of ER (3); every event reaches the ER, reduced, with one status bank per station.
#   B  misplacement: filters start before the ER, the ER is then created at position 1
#      (upstream); status reports rc 2, the prestart hook restarts the filters in front of it.
#   C  filter crash: GEMFILTER_BB is killed (SIGKILL); events still reach the ER (restore=out,
#      SBS station still filtering), and the next prestart restarts BB.
set -u
B=$(cd "$1" && pwd); S=$(cd "$2" && pwd); DB=$(cd "$3" && pwd); NB=${4:-60}
W=$(mktemp -d /tmp/gfkl.XXXX); P=$W/prefix; ETF=/tmp/gfkl_et_$$; PORT=${GF_ET_PORT_BASE:-23931}
T() { perl -e 'alarm shift; exec @ARGV' "$@"; }   # foreground only: backgrounded, $! would be a subshell
# background form: run as  perl ... & ; $! is then the PID of the exec'd program itself
nfail=0
ok()  { echo "ok    $*"; }
bad() { echo "FAIL  $*"; nfail=$((nfail + 1)); }
expect_rc() { local want=$1 got=$2; shift 2; [ "$got" -eq "$want" ] && ok "$* (rc=$got)" || bad "$* (rc=$got, want $want)"; }
fsize() { stat -f%z "$1" 2>/dev/null || stat -c%s "$1"; }
nstatus() { "$P/bin/gemfilter_file" -i "$1" --list-banks 2>/dev/null | awk -F, '$3=="0x4746"{n+=$4}END{print n+0}'; }
pos_of() { sed -n "s/^station=$1 exists=1 position=\([0-9-]*\).*/\1/p" "$2"; }

cmake --install "$B" --prefix "$P" > "$W/install.log" 2>&1 || { cat "$W/install.log"; echo FAIL install; exit 1; }
cp "$P/etc/gemfilter_sidis.env.example" "$P/etc/gemfilter_sidis.env"
"$P/bin/gemfilter_bench" --synth "$DB" "$W/in.evio" "$NB" 0.15 > /dev/null 2>&1 || { echo "FAIL synth"; exit 1; }
NEV=$((NB + 3)); INS=$(fsize "$W/in.evio")
export GF_DB_DIR=$DB GF_DB_DATE="2024-10-01 00:00:00" GF_SBS_SEED=vtp GF_RUN_DIR=$W/run GF_LOG_DIR=$W/log \
       EXPID=kitlive GF_EXPID=kitlive GF_ET_FILE=$ETF GF_ET_UDP_PORT=$((PORT + 1)) GF_ER_STATION=ER \
       GF_BB_THREADS=2 GF_SBS_THREADS=2 GF_PRESCALE=0 GF_ATTACH_WAIT_S=30 GF_STATS_S=2 GF_PROBE_TIMEOUT_S=2
unset GF_PREFIX GF_ENV_FILE
CTL=$P/bin/gemfilter_ctl; HOOK=$P/bin/gf_rc_hook.sh

cleanup() {
  "$CTL" stop > /dev/null 2>&1
  [ -n "${DP:-}" ] && kill "$DP" 2>/dev/null
  if [ -n "${EP:-}" ]; then
    kill "$EP" 2>/dev/null
    for i in 1 2 3 4 5 6 7 8 9 10; do kill -0 "$EP" 2>/dev/null || break; sleep 0.5; done
    kill -KILL "$EP" 2>/dev/null
  fi
  wait 2>/dev/null; rm -f "$ETF"
}
trap cleanup EXIT

perl -e 'alarm shift; exec @ARGV' 380 "$P/bin/et_start" -f "$ETF" -n 256 -s 1500000 -d -p "$PORT" -u $((PORT + 1)) > "$W/et_start.log" 2>&1 & EP=$!
sleep 2
kill -0 $EP 2>/dev/null && ok "et_start running" || { cat "$W/et_start.log"; echo "FAIL et_start"; exit 1; }
"$P/bin/gf_et_status" -f "$ETF" --udp-port $((PORT + 1)); expect_rc 0 $? "gf_et_status probes the ET system"

# ---------------- A: normal run-control cycle
perl -e 'alarm shift; exec @ARGV' 200 "$P/bin/gf_et_drain" -f "$ETF" -o "$W/outA.evio" -n $NEV --timeout 60 --station ER > "$W/drainA.log" 2>&1 & DP=$!
sleep 1
"$HOOK" prestart TEST 1 s; expect_rc 0 $? "A: prestart hook"
"$CTL" status > "$W/stA.txt" 2>&1; expect_rc 0 $? "A: status after prestart"
pb=$(pos_of GEMFILTER_BB "$W/stA.txt"); ps_=$(pos_of GEMFILTER_SBS "$W/stA.txt"); pe=$(pos_of ER "$W/stA.txt")
[ "$pb" = 1 ] && [ "$ps_" = 2 ] && [ "$pe" = 3 ] && ok "A: positions BB=$pb SBS=$ps_ ER=$pe" || bad "A: positions BB=$pb SBS=$ps_ ER=$pe"
grep -q 'prestart_rc=0' "$W/log/runs/run_1.txt" && ok "A: run record" || bad "A: run record"
"$HOOK" go TEST 1 s
T 120 "$P/bin/gf_et_feed" -f "$ETF" -i "$W/in.evio" > "$W/feedA.log" 2>&1
wait $DP; expect_rc 0 $? "A: ER received all $NEV events"; DP=""
"$HOOK" end TEST 1 s
OUTS=$(fsize "$W/outA.evio"); n=$(nstatus "$W/outA.evio")
[ "$OUTS" -lt $((INS / 4)) ] && ok "A: reduction $INS -> $OUTS bytes" || bad "A: no reduction $INS -> $OUTS"
[ "$n" -eq $((2 * NB)) ] && ok "A: $n status banks (BB + SBS on every physics event)" || bad "A: $n status banks, want $((2 * NB))"
grep -q '^end: ' "$W/log/runs/run_1.txt" && ok "A: end recorded" || bad "A: end record"

# ---------------- B: ER created upstream of already-running filters
# (fresh ER name: the A station "ER" stays idle in the chain, idle stations are bypassed)
"$CTL" stop > /dev/null
export GF_ER_STATION=ER_B
"$CTL" start > /dev/null; expect_rc 0 $? "B: filters started before the ER"
sleep 2
perl -e 'alarm shift; exec @ARGV' 200 "$P/bin/gf_et_drain" -f "$ETF" -o "$W/outB.evio" -n $NEV --timeout 60 --station ER_B --position 1 > "$W/drainB.log" 2>&1 & DP=$!
sleep 2
"$CTL" status > "$W/stB0.txt" 2>&1; expect_rc 2 $? "B: status detects ER upstream of the filters"
"$HOOK" prestart TEST 2 s; expect_rc 0 $? "B: prestart hook"
grep -q 'restarting them' "$W/log/rc_hook.log" && ok "B: hook restarted misplaced filters" || bad "B: no restart logged"
"$CTL" status > "$W/stB.txt" 2>&1; expect_rc 0 $? "B: status after recovery"
pe=$(pos_of ER_B "$W/stB.txt"); pb=$(pos_of GEMFILTER_BB "$W/stB.txt"); ps_=$(pos_of GEMFILTER_SBS "$W/stB.txt")
[ "$pb" -lt "$pe" ] && [ "$ps_" -lt "$pe" ] && ok "B: positions BB=$pb SBS=$ps_ ER=$pe" || bad "B: positions BB=$pb SBS=$ps_ ER=$pe"
T 120 "$P/bin/gf_et_feed" -f "$ETF" -i "$W/in.evio" > "$W/feedB.log" 2>&1
wait $DP; expect_rc 0 $? "B: ER received all $NEV events"; DP=""
n=$(nstatus "$W/outB.evio")
[ "$n" -eq $((2 * NB)) ] && ok "B: $n status banks" || bad "B: $n status banks, want $((2 * NB))"

# ---------------- C: BB filter crash
perl -e 'alarm shift; exec @ARGV' 200 "$P/bin/gf_et_drain" -f "$ETF" -o "$W/outC.evio" -n $NEV --timeout 60 --station ER_B > "$W/drainC.log" 2>&1 & DP=$!
sleep 1
kill -KILL "$(cat "$W/run/bb.pid")"; sleep 1
"$CTL" status bb > /dev/null 2>&1; [ $? -ne 0 ] && ok "C: status reports BB down" || bad "C: status after kill"
T 120 "$P/bin/gf_et_feed" -f "$ETF" -i "$W/in.evio" > "$W/feedC.log" 2>&1
wait $DP; expect_rc 0 $? "C: ER received all $NEV events with BB dead"; DP=""
OUTS=$(fsize "$W/outC.evio"); n=$(nstatus "$W/outC.evio")
[ "$OUTS" -lt $((INS / 4)) ] && ok "C: SBS station still reduces ($OUTS bytes)" || bad "C: $OUTS bytes"
[ "$n" -eq "$NB" ] && ok "C: $n status banks (SBS only)" || bad "C: $n status banks, want $NB"
"$HOOK" prestart TEST 3 s; expect_rc 0 $? "C: next prestart restarts BB"
"$CTL" status > /dev/null 2>&1; expect_rc 0 $? "C: status after restart"

GF_STOP_ON_RESET=1 "$HOOK" reset TEST 3 s
[ -f "$W/run/bb.pid" ] || [ -f "$W/run/sbs.pid" ] && bad "filters still running after reset" || ok "reset stopped the filters"

echo "---- rc_hook.log (tail)"; tail -8 "$W/log/rc_hook.log"
if [ $nfail -eq 0 ]; then echo PASS; trap - EXIT; cleanup; rm -rf "$W"; exit 0; fi
echo "FAIL: $nfail checks (work dir $W)"
for f in "$W"/log/gemfilter_*.log "$W"/drain*.log "$W"/feed*.log; do echo "---- $f"; tail -6 "$f"; done
exit 1
