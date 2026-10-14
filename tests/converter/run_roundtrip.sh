#!/usr/bin/env bash
# converter.roundtrip : SYNTHETIC libsbsdig-layout tree -> .sdg -> EVIO -> gemfilter decode == input.
# usage: run_roundtrip.sh <python3> <tools-src-dir> <sbsdig2evio-bin-dir> <gemfilter_file> <DB dir> <libsbsdig conf> <workdir>
# Exit 77 (CTest SKIP) if uproot/awkward are not importable.
set -euo pipefail
PY=$1; SRC=$2; BIN=$3; GFF=$4; DB=$5; CONF=$6; W=$7
"$PY" -c "import uproot, awkward" 2>/dev/null || { echo "uproot/awkward not available: SKIP"; exit 77; }
mkdir -p "$W"; cd "$W"
DATE="2023-06-01 00:00:00"
"$PY" "$SRC/make_synthetic_sbsdig_tree.py" -o syn.root -n 23 --with-sig
# 1) truth from the (patched-libsbsdig) signal branch
"$PY" "$SRC/sbsdig_extract.py" -i syn.root -o syn.sdg --truth sig
"$BIN/sbsdig2evio" -i syn.sdg -o syn.evio --db "$DB" --date "$DATE"
"$BIN/sbsdig2evio_verify" -i syn.evio --sdg syn.sdg --db "$DB" --date "$DATE" --truth syn.evio.truth.tsv
# 2) truth from g4sbs hit geometry must agree with the branch truth on this synthetic sample
"$PY" "$SRC/sbsdig_extract.py" -i syn.root -o synh.sdg --truth hits --sbsdig-db "$CONF" --hit-margin 1
"$BIN/sbsdig2evio" -i synh.sdg -o synh.evio --db "$DB" --date "$DATE"
"$PY" - <<'PYEOF'
def load(f):
    d = {}
    for l in open(f):
        if l[0] == '#': continue
        t = l.rstrip('\n').split('\t'); d[(t[0], t[1], t[3], t[4], t[5])] = int(t[9])
    return d
a, b = load('syn.evio.truth.tsv'), load('synh.evio.truth.tsv')
assert a.keys() == b.keys(), "truth sidecars cover different strips"
nsig = sum(v == 1 for v in a.values()); bad = sum(a[k] != b[k] for k in a)
print(f"truth: {len(a)} strips, {nsig} signal (branch), {bad} disagreements (hit geometry)")
assert nsig > 0 and bad == 0
PYEOF
# 3) partial-block / max-events path and HCal-less configuration
"$BIN/sbsdig2evio" -i syn.sdg -o syn7.evio --db "$DB" --date "$DATE" --calo bb.sh,bb.ps --max-events 7
"$PY" - <<'PYEOF'
import struct
# re-write a 7-event .sdg so the verifier compares like with like
src = open('syn.sdg', 'rb').read(); out = bytearray(src[:16]); pos = 16; n = 0
while n < 7:
    m, entry, ng, npm = struct.unpack_from('<IIII', src, pos); p = pos + 16
    for _ in range(ng): p += 8 + 2 * src[p + 4]
    for _ in range(npm):
        ns, nt = struct.unpack_from('<HH', src, p + 4); p += 8 + 2 * ns + 4 * nt
    out += src[pos:p]; pos = p; n += 1
out += struct.pack('<II', 0x21444E45, 7); open('syn7.sdg', 'wb').write(out)
PYEOF
"$BIN/sbsdig2evio_verify" -i syn7.evio --sdg syn7.sdg --db "$DB" --date "$DATE" --calo bb.sh,bb.ps --truth syn7.evio.truth.tsv
# 4) the production file driver reads the converted file
"$GFF" -i syn.evio --list-banks | grep -q "3561" || { echo "gemfilter_file did not see MPD banks"; exit 1; }
echo "converter.roundtrip: PASS"
