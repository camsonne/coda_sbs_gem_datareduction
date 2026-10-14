# JLab ET and evio C libraries for gemfilter.  No third-party sources are vendored here;
# see third_party/README.md for the recipe.  Provides:
#   gemfilter::et       ET C library (local + remote client, and ET system side)
#   gemfilter::evio     evio C library (evOpen/evRead/evWrite, v4 read, v6 read/write)
#   gemfilter::et_nonet (only when built from source) ET with the TCP/UDP server threads
#                       "parked" instead of calling exit() when sockets cannot be opened.
#                       TEST-ONLY: lets an in-process ET integration test run in sandboxes
#                       that forbid bind() (e.g. the macOS app sandbox used for development).
#   GEMFILTER_ET_START_EXE  path to et_start (for the multi-process ET test), if available.

option(GEMFILTER_USE_CODA "Use ET/evio installed under $CODA (CODA3 installation)" OFF)
set(GEMFILTER_ET_SOURCE_DIR   "" CACHE PATH "Unpacked JeffersonLab/et source tree")
set(GEMFILTER_EVIO_SOURCE_DIR "" CACHE PATH "Unpacked JeffersonLab/evio source tree")
option(GEMFILTER_FETCH_DEPS "Download ET/evio sources (codeload.github.com) if not given" ON)
set(GEMFILTER_ET_URL   "https://codeload.github.com/JeffersonLab/et/tar.gz/refs/heads/main"
    CACHE STRING "ET source tarball URL")
set(GEMFILTER_EVIO_URL "https://codeload.github.com/JeffersonLab/evio/tar.gz/refs/heads/main"
    CACHE STRING "evio source tarball URL")

set(GEMFILTER_ET_START_EXE "" CACHE INTERNAL "et_start executable")
set(GEMFILTER_HAVE_ET_NONET OFF CACHE INTERNAL "et_nonet test library available")

set(_gf_arch "${CMAKE_SYSTEM_NAME}-${CMAKE_SYSTEM_PROCESSOR}")

# ----------------------------------------------------------------------------------------
# Mode 1: installed CODA libraries ($CODA/<arch>/lib, $CODA/common/include)
# ----------------------------------------------------------------------------------------
if(GEMFILTER_USE_CODA)
  if(NOT DEFINED ENV{CODA})
    message(FATAL_ERROR "GEMFILTER_USE_CODA=ON but $CODA is not set (source the CODA setup script)")
  endif()
  set(_coda $ENV{CODA})
  set(_hints_lib ${_coda}/${_gf_arch}/lib ${_coda}/Linux-x86_64/lib ${_coda}/lib $ENV{CODA_LIB})
  set(_hints_inc ${_coda}/common/include ${_coda}/${_gf_arch}/include ${_coda}/include)
  find_library(GF_ET_LIBRARY   NAMES et   HINTS ${_hints_lib} REQUIRED)
  find_library(GF_EVIO_LIBRARY NAMES evio HINTS ${_hints_lib} REQUIRED)
  find_path(GF_ET_INCLUDE   et.h   HINTS ${_hints_inc} REQUIRED)
  find_path(GF_EVIO_INCLUDE evio.h HINTS ${_hints_inc} REQUIRED)
  add_library(gf_et_imported UNKNOWN IMPORTED GLOBAL)
  set_target_properties(gf_et_imported PROPERTIES IMPORTED_LOCATION ${GF_ET_LIBRARY}
    INTERFACE_INCLUDE_DIRECTORIES ${GF_ET_INCLUDE})
  target_link_libraries(gf_et_imported INTERFACE Threads::Threads ${CMAKE_DL_LIBS} m)
  add_library(gf_evio_imported UNKNOWN IMPORTED GLOBAL)
  set_target_properties(gf_evio_imported PROPERTIES IMPORTED_LOCATION ${GF_EVIO_LIBRARY}
    INTERFACE_INCLUDE_DIRECTORIES ${GF_EVIO_INCLUDE})
  target_link_libraries(gf_evio_imported INTERFACE Threads::Threads m)
  add_library(gemfilter::et   ALIAS gf_et_imported)
  add_library(gemfilter::evio ALIAS gf_evio_imported)
  find_program(_gf_et_start et_start HINTS ${_coda}/${_gf_arch}/bin ${_coda}/bin)
  if(_gf_et_start)
    set(GEMFILTER_ET_START_EXE ${_gf_et_start} CACHE INTERNAL "et_start executable")
  endif()
  message(STATUS "gemfilter: using CODA ET  ${GF_ET_LIBRARY}")
  message(STATUS "gemfilter: using CODA evio ${GF_EVIO_LIBRARY}")
  return()
endif()

# ----------------------------------------------------------------------------------------
# Mode 2/3: build from source (given directory, or downloaded tarball)
# ----------------------------------------------------------------------------------------
include(FetchContent)
function(_gf_get_source var name url)
  if(${var} AND EXISTS ${${var}})
    return()
  endif()
  if(NOT GEMFILTER_FETCH_DEPS)
    message(FATAL_ERROR "${name}: set GEMFILTER_${name}_SOURCE_DIR or enable GEMFILTER_FETCH_DEPS "
                        "or GEMFILTER_USE_CODA")
  endif()
  string(TOLOWER ${name} lname)
  FetchContent_Declare(gf_${lname}_src URL ${url})
  FetchContent_GetProperties(gf_${lname}_src)
  if(NOT gf_${lname}_src_POPULATED)
    message(STATUS "gemfilter: downloading ${name} from ${url}")
    if(POLICY CMP0169)
      cmake_policy(SET CMP0169 OLD)   # FetchContent_Populate without add_subdirectory
    endif()
    FetchContent_Populate(gf_${lname}_src)
  endif()
  set(${var} ${gf_${lname}_src_SOURCE_DIR} PARENT_SCOPE)
endfunction()

_gf_get_source(GEMFILTER_ET_SOURCE_DIR   ET   ${GEMFILTER_ET_URL})
_gf_get_source(GEMFILTER_EVIO_SOURCE_DIR EVIO ${GEMFILTER_EVIO_URL})

set(_et_src ${GEMFILTER_ET_SOURCE_DIR}/src/libsrc)
set(_evio_src ${GEMFILTER_EVIO_SOURCE_DIR}/src/libsrc)
if(NOT EXISTS ${_et_src}/et.h)
  message(FATAL_ERROR "ET sources not found under ${GEMFILTER_ET_SOURCE_DIR}")
endif()
if(NOT EXISTS ${_evio_src}/evio.h)
  message(FATAL_ERROR "evio sources not found under ${GEMFILTER_EVIO_SOURCE_DIR}")
endif()
message(STATUS "gemfilter: ET sources   ${GEMFILTER_ET_SOURCE_DIR}")
message(STATUS "gemfilter: evio sources ${GEMFILTER_EVIO_SOURCE_DIR}")

# Same source list as et/CMakeLists.txt SOURCE_FILES (ET 16.x); built static here.
set(_et_names et_attachment et_bridge et_common et_data et_event et_init et_local et_mem
              et_network et_noshare et_openconfig et_readwrite et_remote et_server
              et_statconfig et_station et_sysconfig et_system etCommonNetwork et_fifo)
set(_et_files "")
foreach(n ${_et_names})
  list(APPEND _et_files ${_et_src}/${n}.c)
endforeach()

function(_gf_third_party_flags tgt)
  set_target_properties(${tgt} PROPERTIES C_STANDARD 11 POSITION_INDEPENDENT_CODE ON)
  # Third-party code: do not inherit gemfilter warning flags; silence its own warnings.
  target_compile_options(${tgt} PRIVATE -w)
endfunction()

add_library(gf_et STATIC ${_et_files})
target_include_directories(gf_et SYSTEM PUBLIC ${_et_src})
target_link_libraries(gf_et PUBLIC Threads::Threads ${CMAKE_DL_LIBS} m)
_gf_third_party_flags(gf_et)
add_library(gemfilter::et ALIAS gf_et)

file(GLOB _evio_files ${_evio_src}/*.c)
add_library(gf_evio STATIC ${_evio_files})
target_include_directories(gf_evio SYSTEM PUBLIC ${_evio_src})
target_link_libraries(gf_evio PUBLIC Threads::Threads m)
_gf_third_party_flags(gf_evio)
add_library(gemfilter::evio ALIAS gf_evio)

# et_start (stock ET system daemon) for the multi-process integration test / local runs.
if(EXISTS ${GEMFILTER_ET_SOURCE_DIR}/src/execsrc/et_start.c)
  add_executable(gf_et_start ${GEMFILTER_ET_SOURCE_DIR}/src/execsrc/et_start.c)
  set_target_properties(gf_et_start PROPERTIES OUTPUT_NAME et_start
                        RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/bin)
  target_link_libraries(gf_et_start PRIVATE gf_et)
  _gf_third_party_flags(gf_et_start)
  set(GEMFILTER_ET_START_EXE $<TARGET_FILE:gf_et_start> CACHE INTERNAL "et_start executable")
  install(TARGETS gf_et_start RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR})
endif()

# ---- TEST-ONLY "nonet" ET variant -------------------------------------------------------
# Two textual edits to a COPY of et_server.c: when the TCP listen socket (et_netserver) or the
# UDP multicast socket (et_cast_thread) cannot be opened, the thread signals "started" and
# sleeps forever instead of calling exit(1).  Local (same-process) clients are unaffected.
set(_srv_in  ${_et_src}/et_server.c)
set(_srv_out ${CMAKE_CURRENT_BINARY_DIR}/et_nonet/et_server_nonet.c)
file(READ ${_srv_in} _srv)
set(_a_old [=[            et_logmsg("SEVERE", "et_cast_thread: problem opening socket\n");
        }
        exit(1);]=])
set(_a_new [=[            et_logmsg("SEVERE", "et_cast_thread: problem opening socket\n");
        }
        /* gemfilter et_nonet (test build): park the thread instead of exit(1) */
        for (;;) sleep(3600);]=])
set(_b_old [=[            et_logmsg("SEVERE", "et_netserver: specified port is busy, cannot start server thread\n");
        }
        exit(1);]=])
set(_b_new [=[            et_logmsg("SEVERE", "et_netserver: specified port is busy, cannot start server thread\n");
        }
        /* gemfilter et_nonet (test build): report started, then park instead of exit(1) */
        etid->race = -1;
        for (;;) sleep(3600);]=])
string(FIND "${_srv}" "${_a_old}" _pa)
string(FIND "${_srv}" "${_b_old}" _pb)
if(_pa EQUAL -1 OR _pb EQUAL -1)
  message(WARNING "gemfilter: et_server.c layout changed; et_nonet test library not built")
else()
  string(REPLACE "${_a_old}" "${_a_new}" _srv "${_srv}")
  string(REPLACE "${_b_old}" "${_b_new}" _srv "${_srv}")
  file(WRITE ${_srv_out}.tmp "#include <unistd.h>\n${_srv}")
  configure_file(${_srv_out}.tmp ${_srv_out} COPYONLY)
  set(_et_nonet_files ${_et_files})
  list(REMOVE_ITEM _et_nonet_files ${_srv_in})
  list(APPEND _et_nonet_files ${_srv_out})
  add_library(gf_et_nonet STATIC EXCLUDE_FROM_ALL ${_et_nonet_files})
  target_include_directories(gf_et_nonet SYSTEM PUBLIC ${_et_src})
  target_link_libraries(gf_et_nonet PUBLIC Threads::Threads ${CMAKE_DL_LIBS} m)
  _gf_third_party_flags(gf_et_nonet)
  add_library(gemfilter::et_nonet ALIAS gf_et_nonet)
  set(GEMFILTER_HAVE_ET_NONET ON CACHE INTERNAL "et_nonet test library available")
endif()
