#!/usr/bin/env bash
# deploy.kit_static: install the build tree into a scratch prefix and exercise the CODA
# deployment kit without a running ET system (no sockets needed).
#   kit_static_test.sh <build dir> <source dir> <test DB dir>
set -u
B=$(cd "$1" && pwd); S=$(cd "$2" && pwd); DB=$(cd "$3" && pwd)
W=$(mktemp -d /tmp/gfks.XXXX); P=$W/prefix
nfail=0
ok()  { echo "ok    $*"; }
bad() { echo "FAIL  $*"; nfail=$((nfail + 1)); }
expect_rc() { local want=$1 got=$2; shift 2; [ "$got" -eq "$want" ] && ok "$* (rc=$got)" || bad "$* (rc=$got, want $want)"; }

cmake --install "$B" --prefix "$P" > "$W/install.log" 2>&1 || { cat "$W/install.log"; echo FAIL install; exit 1; }
for f in gemfilter_ctl gemfilter_check gf_rc_hook.sh install_gemfilter.sh; do
  bash -n "$P/bin/$f" && ok "syntax $f" || bad "syntax $f"
done
for f in "$P/share/gemfilter/gf_common.sh" "$P/etc/setup_gemfilter.sh" "$P/etc/gemfilter_sidis.env.example"; do
  bash -n "$f" && ok "syntax $(basename "$f")" || bad "syntax $(basename "$f")"
done
for t in bb sbs; do [ -f "$P/share/gemfilter/sidis/$t.cfg.in" ] && ok "template $t" || bad "template $t missing"; done

cp "$P/etc/gemfilter_sidis.env.example" "$P/etc/gemfilter_sidis.env"
export GF_DB_DIR=$DB GF_DB_DATE="2024-10-01 00:00:00" GF_SBS_SEED=vtp GF_RUN_DIR=$W/run \
       GF_LOG_DIR=$W/log EXPID=kitstatic GF_EXPID=kitstatic GF_BB_THREADS=1 GF_SBS_THREADS=1 \
       GF_ET_FILE=$W/et_none GF_ATTACH_WAIT_S=2 GF_PROBE_TIMEOUT_S=0
unset GF_PREFIX GF_ENV_FILE

# setup script puts the kit on PATH
( . "$P/etc/setup_gemfilter.sh" >/dev/null 2>&1; command -v gemfilter_ctl ) | grep -q "$P/bin/gemfilter_ctl" \
  && ok "setup_gemfilter.sh PATH" || bad "setup_gemfilter.sh PATH"

# render + command lines
"$P/bin/gemfilter_ctl" render > "$W/render.out"; expect_rc 0 $? "gemfilter_ctl render"
grep -q '@GF_' "$W/run/bb.cfg" "$W/run/sbs.cfg" && bad "unrendered fields" || ok "configs fully rendered"
grep -q '^seed_source = vtp' "$W/run/sbs.cfg" && ok "GF_SBS_SEED applied" || bad "GF_SBS_SEED not applied"
"$P/bin/gemfilter_ctl" cmd > "$W/cmd.out"
grep -q -- '--station GEMFILTER_BB --position 1 ' "$W/cmd.out" && ok "BB station at position 1" || bad "BB command line"
grep -q -- '--station GEMFILTER_SBS --position 2 ' "$W/cmd.out" && ok "SBS station at position 2" || bad "SBS command line"
grep -q -- '--instance-id 0xbb000001' "$W/cmd.out" && grep -q -- '--instance-id 0x5b500002' "$W/cmd.out" \
  && ok "instance ids" || bad "instance ids"
grep -q -- '--fix-position' "$W/cmd.out" && grep -q -- '--restore out' "$W/cmd.out" && ok "fix-position/restore" || bad "fix-position/restore"
GF_BLOCKING=0 "$P/bin/gemfilter_ctl" cmd | grep -q -- '--nonblocking --cue 200' && ok "non-blocking option" || bad "non-blocking option"

# config check of both arms
for a in bb sbs; do
  "$P/bin/gemfilter_file" --check -p "gem:$W/run/$a.cfg" > "$W/check_$a.out" 2>&1
  expect_rc 0 $? "gemfilter_file --check $a"
done
"$P/bin/gemfilter_file" --check -p gem:/nonexistent.cfg > /dev/null 2>&1; expect_rc 1 $? "--check on missing config fails"

# preflight
"$P/bin/gemfilter_check" > "$W/pre.out" 2>&1; expect_rc 0 $? "gemfilter_check (valid site config)"
GF_DB_DIR=/nonexistent "$P/bin/gemfilter_check" > /dev/null 2>&1; expect_rc 1 $? "gemfilter_check with missing DB"
GF_ARMS="bb foo" "$P/bin/gemfilter_check" > /dev/null 2>&1; expect_rc 1 $? "gemfilter_check with unknown arm"
GF_ET_FILE=/tmp/$(printf 'x%.0s' $(seq 1 100)) "$P/bin/gemfilter_check" > /dev/null 2>&1; expect_rc 1 $? "gemfilter_check with ET name > 99 chars"
GF_DB_DATE="not a date" "$P/bin/gemfilter_check" > /dev/null 2>&1; expect_rc 1 $? "gemfilter_check with bad DB date"

# status without ET: no process, ET unreachable -> 3
"$P/bin/gemfilter_ctl" status > "$W/status.out" 2>&1; expect_rc 3 $? "status without ET system"

# run-control hook: disabled, and failure paths without ET (filters cannot attach)
GF_ENABLE=0 "$P/bin/gf_rc_hook.sh" prestart TEST 7 s; expect_rc 0 $? "hook prestart with GF_ENABLE=0"
grep -q 'gemfilter=DISABLED' "$W/log/runs/run_7.txt" && ok "disabled run record" || bad "disabled run record"
"$P/bin/gf_rc_hook.sh" prestart TEST 8 s; expect_rc 0 $? "hook prestart without ET (non-strict)"
grep -q 'prestart_rc=[13]' "$W/log/runs/run_8.txt" && ok "run record has failure rc" || bad "run record rc"
grep -q 'NOT in place' "$W/log/rc_hook.log" && ok "hook logged the failure" || bad "hook log"
GF_STRICT=1 "$P/bin/gf_rc_hook.sh" prestart TEST 9 s; expect_rc 10 $? "hook prestart without ET (strict)"
"$P/bin/gf_rc_hook.sh" end TEST 9 s; expect_rc 0 $? "hook end"
GF_STOP_ON_RESET=1 "$P/bin/gf_rc_hook.sh" reset TEST 9 s; expect_rc 0 $? "hook reset (stop)"
for a in bb sbs; do
  [ -f "$W/run/$a.pid" ] && bad "$a pid file left after stop" || ok "$a stopped"
done
"$P/bin/gf_rc_hook.sh" bogus TEST 9 s; expect_rc 0 $? "hook unknown transition"

if [ $nfail -eq 0 ]; then echo PASS; rm -rf "$W"; exit 0; fi
echo "FAIL: $nfail checks (work dir $W)"; tail -20 "$W/log/rc_hook.log" 2>/dev/null; exit 1
