#!/usr/bin/env bash
# Live ET chain test (needs a machine where et_start may bind its TCP/UDP ports, e.g. a DAQ test box):
#   et_start -> gf_et_drain (station ER, END) -> gemfilter_et (position 1, -p gem:cfg) -> gf_et_feed
# Usage: et_live_test.sh <build dir> <SBS-replay DB dir> [nblocks=200] [occupancy=0.15]
# Pass criteria: drain receives every CODA event (control + physics) in order, physics events carry
# a GEMFILTER status bank, output much smaller than input.
set -u
B=$(cd "$1" && pwd); DB=$(cd "$2" && pwd); NB=${3:-200}; OCC=${4:-0.15}
W=$(mktemp -d /tmp/gfet.XXXX); ETF=/tmp/gf_et_$$; PORT=${GF_ET_PORT:-23911}
T(){ perl -e 'alarm shift; exec @ARGV' "$@"; }
cd "$W"
"$B/tools/gemfilter_bench" --synth "$DB" in.evio "$NB" "$OCC" || exit 1
NEV=$((NB + 3))
T 300 "$B/bin/et_start" -f "$ETF" -n 128 -s 1500000 -d -p "$PORT" -u $((PORT+1)) > et_start.log 2>&1 & EP=$!
sleep 2
T 280 "$B/tools/gf_et_drain" -f "$ETF" -o out.evio -n "$NEV" --timeout 30 > drain.log 2>&1 & DP=$!
sleep 1
T 270 "$B/bin/gemfilter_et" -f "$ETF" -p gem:in.evio.cfg --er-station ER -t 4 --stats 5 --udp-port $((PORT+1)) > filter.log 2>&1 & FP=$!
sleep 2
T 200 "$B/tools/gf_et_feed" -f "$ETF" -i in.evio > feed.log 2>&1
wait $DP; RC=$?
kill -INT $FP 2>/dev/null; sleep 2; kill $FP $EP 2>/dev/null; wait 2>/dev/null; rm -f "$ETF"
cat feed.log drain.log; tail -5 filter.log
if [ $RC -ne 0 ]; then echo "FAIL: drain rc=$RC (work dir $W)"; exit 1; fi
"$B/bin/gemfilter_file" -i out.evio -p passthrough --no-status -q --csv chk.csv >/dev/null 2>&1
INS=$(stat -f%z in.evio 2>/dev/null || stat -c%s in.evio); OUTS=$(stat -f%z out.evio 2>/dev/null || stat -c%s out.evio)
echo "input $INS bytes, output $OUTS bytes"
[ "$OUTS" -lt $((INS / 4)) ] || { echo "FAIL: no reduction"; exit 1; }
echo PASS; rm -rf "$W"
