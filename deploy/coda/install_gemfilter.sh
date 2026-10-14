#!/usr/bin/env bash
# install_gemfilter.sh - build, test and install gemfilter + the CODA deployment kit.
#
#   deploy/coda/install_gemfilter.sh --prefix /home/coda/gemfilter [--mode coda|fetch|src]
#        [--et-src DIR --evio-src DIR] [--build-dir DIR] [--build-type Release] [-j N]
#        [--no-test] [--live-test]
#
#   --mode coda   link the ET/evio libraries of the CODA installation ($CODA, default when set).
#                 REQUIRED for production: a local ET client must use the same ET version as
#                 the EB's ET system (shared-memory layout).
#   --mode fetch  download JeffersonLab/et and evio sources (codeload.github.com); tests/CI.
#   --mode src    use unpacked sources given by --et-src / --evio-src.
#   --live-test   also run the live ET chain tests (need a host where et_start may bind ports).
#
# Never overwrites an existing <prefix>/etc/gemfilter_sidis.env.
set -eu
here=$(cd "$(dirname "$0")" && pwd); src=$(cd "$here/../.." && pwd)
prefix=""; mode=""; etsrc=""; eviosrc=""; bdir=""; btype=Release; jobs=""; test=1; live=0
while [ $# -gt 0 ]; do
  case "$1" in
    --prefix) prefix=$2; shift;; --mode) mode=$2; shift;;
    --et-src) etsrc=$2; shift;; --evio-src) eviosrc=$2; shift;;
    --build-dir) bdir=$2; shift;; --build-type) btype=$2; shift;;
    -j) jobs=$2; shift;; --no-test) test=0;; --live-test) live=1;;
    -h|--help) sed -n '2,19p' "$0"; exit 0;;
    *) echo "unknown option $1" >&2; exit 2;;
  esac; shift
done
[ -n "$prefix" ] || { echo "--prefix is required" >&2; exit 2; }
[ -n "$mode" ] || { if [ -n "${CODA:-}" ]; then mode=coda; else mode=fetch; fi; }
bdir=${bdir:-$src/build-deploy}
command -v cmake >/dev/null || { echo "cmake not found (>= 3.16 needed)" >&2; exit 1; }
gen=""; command -v ninja >/dev/null && gen="-G Ninja"
echo "== gemfilter install: src=$src prefix=$prefix mode=$mode build=$bdir ($btype)"
echo "   cmake $(cmake --version | head -1 | awk '{print $3}'), compiler ${CXX:-$(command -v c++)}"

opts="-DCMAKE_BUILD_TYPE=$btype -DCMAKE_INSTALL_PREFIX=$prefix"
case "$mode" in
  coda)  [ -n "${CODA:-}" ] || { echo "--mode coda needs \$CODA (source the CODA setup)" >&2; exit 1; }
         opts="$opts -DGEMFILTER_USE_CODA=ON";;
  fetch) opts="$opts -DGEMFILTER_FETCH_DEPS=ON";;
  src)   [ -d "$etsrc" ] && [ -d "$eviosrc" ] || { echo "--mode src needs --et-src and --evio-src" >&2; exit 1; }
         opts="$opts -DGEMFILTER_ET_SOURCE_DIR=$etsrc -DGEMFILTER_EVIO_SOURCE_DIR=$eviosrc";;
  *) echo "bad --mode $mode" >&2; exit 2;;
esac
[ $live -eq 1 ] && opts="$opts -DGEMFILTER_LIVE_ET_TEST=ON"
# shellcheck disable=SC2086
cmake -S "$src" -B "$bdir" $gen $opts
cmake --build "$bdir" --parallel ${jobs}
if [ $test -eq 1 ]; then ctest --test-dir "$bdir" --output-on-failure; fi
cmake --install "$bdir"

# build record (read by gemfilter_check)
etlib=$(sed -n 's/^GF_ET_LIBRARY:FILEPATH=//p' "$bdir/CMakeCache.txt")
[ -n "$etlib" ] || etlib="(ET built from source, mode=$mode${etsrc:+, $etsrc})"
etver=""
for h in "$(sed -n 's/^GF_ET_INCLUDE:PATH=//p' "$bdir/CMakeCache.txt")" "$(find "$bdir/_deps" -name et_private.h 2>/dev/null | head -1 | xargs dirname 2>/dev/null)"; do
  [ -n "$h" ] && [ -f "$h/et_private.h" ] && etver=$(awk '/define ET_VERSION /{v=$3}/define ET_VERSION_MINOR /{m=$3}END{print v"."m}' "$h/et_private.h")
done
mkdir -p "$prefix/share/gemfilter"
{ echo "built=$(date '+%Y-%m-%d %H:%M:%S') host=$(hostname)"; echo "source=$src"; echo "build_type=$btype"
  echo "et_mode=$mode"; echo "et_library=$etlib"; echo "et_version=${etver:-unknown}"
  echo "coda=${CODA:-}"; } > "$prefix/share/gemfilter/build_info.txt"

if [ ! -f "$prefix/etc/gemfilter_sidis.env" ]; then
  cp "$prefix/etc/gemfilter_sidis.env.example" "$prefix/etc/gemfilter_sidis.env"
  echo "== created $prefix/etc/gemfilter_sidis.env: edit the [SET]/[VERIFY] lines"
else
  echo "== kept existing $prefix/etc/gemfilter_sidis.env (compare with gemfilter_sidis.env.example)"
fi
echo "== installed.  Next:"
echo "   source $prefix/etc/setup_gemfilter.sh && gemfilter_check"
