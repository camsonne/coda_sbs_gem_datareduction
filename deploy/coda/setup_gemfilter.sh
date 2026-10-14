# setup_gemfilter.sh - source AFTER the CODA3 setup (bash/zsh):
#   source $CODA/.setup   (or your coda_scripts setupCODA3.bash)
#   source <prefix>/etc/setup_gemfilter.sh
# Puts the gemfilter tools on PATH and loads the site configuration (gemfilter_sidis.env).
if [ -n "${BASH_SOURCE:-}" ]; then _gf_here=${BASH_SOURCE[0]}; else _gf_here=${(%):-%x}; fi
export GF_PREFIX=${GF_PREFIX:-$(cd "$(dirname "$_gf_here")/.." && pwd)}
unset _gf_here
case ":$PATH:" in *":$GF_PREFIX/bin:"*) ;; *) export PATH="$GF_PREFIX/bin:$PATH";; esac
export GF_ENV_FILE=${GF_ENV_FILE:-$GF_PREFIX/etc/gemfilter_sidis.env}
if [ -f "$GF_ENV_FILE" ]; then . "$GF_ENV_FILE"; else echo "setup_gemfilter: no $GF_ENV_FILE" >&2; fi
[ -z "${CODA:-}" ] && echo "setup_gemfilter: WARNING: \$CODA not set (source the CODA setup first)" >&2
true
