# gemfilter_evioio: CODA3 EVIO event access/rewrite (in-memory, no deps) + file I/O (evio C).
cmake_minimum_required(VERSION 3.16)
if(NOT DEFINED GEMFILTER_ROOT)
  get_filename_component(GEMFILTER_ROOT "${CMAKE_CURRENT_LIST_DIR}/.." ABSOLUTE)
endif()

option(GEMFILTER_WITH_EVIO "Build EVIO file reader / v6 writer on the JLab evio C library" ON)
set(GEMFILTER_EVIO_SOURCE_DIR "" CACHE PATH
    "evio source tree (e.g. evio-main, evio 6.x): src/libsrc/*.c compiled into gemfilter_evio_c")
set(GEMFILTER_EVIO_ROOT "" CACHE PATH "Installed evio prefix containing include/evio.h and lib/libevio")

set(_gf_evio_target "")
if(GEMFILTER_WITH_EVIO)
  if(TARGET gemfilter::evio)
    set(_gf_evio_target gemfilter::evio)
  elseif(TARGET evio)
    set(_gf_evio_target evio)
  elseif(TARGET gemfilter_evio_c)
    set(_gf_evio_target gemfilter_evio_c)
  elseif(GEMFILTER_EVIO_SOURCE_DIR AND EXISTS "${GEMFILTER_EVIO_SOURCE_DIR}/src/libsrc/evio.c")
    # Same recipe as Podd's bundled evio build: the four C sources, no other dependency.
    enable_language(C)
    find_package(Threads REQUIRED)
    set(_d "${GEMFILTER_EVIO_SOURCE_DIR}/src/libsrc")
    add_library(gemfilter_evio_c STATIC ${_d}/evio.c ${_d}/eviofmt.c ${_d}/eviofmtswap.c ${_d}/evioswap.c)
    target_include_directories(gemfilter_evio_c PUBLIC ${_d})
    set_target_properties(gemfilter_evio_c PROPERTIES C_STANDARD 11 POSITION_INDEPENDENT_CODE ON)
    if(CMAKE_C_COMPILER_ID MATCHES "GNU|Clang")
      target_compile_options(gemfilter_evio_c PRIVATE -w)
    endif()
    target_link_libraries(gemfilter_evio_c PUBLIC Threads::Threads)
    set(_gf_evio_target gemfilter_evio_c)
  else()
    find_path(GEMFILTER_EVIO_INCLUDE_DIR evio.h
      HINTS ${GEMFILTER_EVIO_ROOT} $ENV{EVIO} $ENV{CODA}
      PATH_SUFFIXES include common/include)
    find_library(GEMFILTER_EVIO_LIBRARY NAMES evio
      HINTS ${GEMFILTER_EVIO_ROOT} $ENV{EVIO} $ENV{CODA}
      PATH_SUFFIXES lib lib64 ${CMAKE_SYSTEM_NAME}-${CMAKE_SYSTEM_PROCESSOR}/lib)
    if(GEMFILTER_EVIO_INCLUDE_DIR AND GEMFILTER_EVIO_LIBRARY)
      find_package(Threads REQUIRED)
      add_library(gemfilter_evio_c UNKNOWN IMPORTED)
      set_target_properties(gemfilter_evio_c PROPERTIES
        IMPORTED_LOCATION "${GEMFILTER_EVIO_LIBRARY}"
        INTERFACE_INCLUDE_DIRECTORIES "${GEMFILTER_EVIO_INCLUDE_DIR}"
        INTERFACE_LINK_LIBRARIES Threads::Threads)
      set(_gf_evio_target gemfilter_evio_c)
    endif()
  endif()
  if(_gf_evio_target)
    message(STATUS "gemfilter_evioio: evio C library = ${_gf_evio_target}")
  else()
    message(WARNING "gemfilter_evioio: evio C library not found; EvioFileReader and EVIO v6 output disabled. "
                    "Set GEMFILTER_EVIO_SOURCE_DIR or GEMFILTER_EVIO_ROOT.")
  endif()
endif()

add_library(gemfilter_evioio
  src/evio_event.cpp
  src/evio_swap.cpp
  src/evio_file.cpp
  src/synthetic_event.cpp)
add_library(gemfilter::evioio ALIAS gemfilter_evioio)
target_include_directories(gemfilter_evioio PUBLIC
  $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
  $<BUILD_INTERFACE:${GEMFILTER_ROOT}/include>)
target_compile_features(gemfilter_evioio PUBLIC cxx_std_17)
set_target_properties(gemfilter_evioio PROPERTIES CXX_EXTENSIONS OFF POSITION_INDEPENDENT_CODE ON)
if(CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang")
  target_compile_options(gemfilter_evioio PRIVATE -Wall -Wextra -Wpedantic)
endif()
if(_gf_evio_target)
  target_link_libraries(gemfilter_evioio PUBLIC ${_gf_evio_target})
  target_compile_definitions(gemfilter_evioio PUBLIC GEMFILTER_HAVE_EVIO=1)
  set(GEMFILTER_HAVE_EVIO ON CACHE INTERNAL "evio C library available to gemfilter")
else()
  target_compile_definitions(gemfilter_evioio PUBLIC GEMFILTER_HAVE_EVIO=0)
  set(GEMFILTER_HAVE_EVIO OFF CACHE INTERNAL "evio C library available to gemfilter")
endif()
