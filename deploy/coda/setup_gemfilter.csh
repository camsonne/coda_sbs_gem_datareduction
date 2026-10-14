# setup_gemfilter.csh - source AFTER the CODA3 setup (csh/tcsh):
#   source <prefix>/etc/setup_gemfilter.csh <prefix>
# The gemfilter scripts are bash and read gemfilter_sidis.env themselves; csh users only
# need PATH and GF_PREFIX.
if ( $#argv >= 1 ) then
  setenv GF_PREFIX $argv[1]
else if ( ! $?GF_PREFIX ) then
  echo "setup_gemfilter.csh: give the install prefix as argument"
  exit 1
endif
setenv PATH ${GF_PREFIX}/bin:${PATH}
setenv GF_ENV_FILE ${GF_PREFIX}/etc/gemfilter_sidis.env
if ( ! $?CODA ) echo "setup_gemfilter.csh: WARNING: CODA not set (source the CODA setup first)"
