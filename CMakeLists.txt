# gemfilter — online GEM data suppression for SBS (CODA3 software filter).
# Top-level build (module interfaces: docs/INTERFACES.md).
#
#   cmake -S . -B build -G Ninja            # Release by default
#   cmake --build build && ctest --test-dir build --output-on-failure
#
# Third-party (JLab ET + evio C libraries) — choose ONE (see third_party/README.md):
#   -DGEMFILTER_USE_CODA=ON                  use installed libs under $CODA (JLab DAQ machines)
#   -DGEMFILTER_ET_SOURCE_DIR=... -DGEMFILTER_EVIO_SOURCE_DIR=...   build from unpacked sources
#   (default) download main-branch tarballs from GitHub (codeload) at configure time
cmake_minimum_required(VERSION 3.16)
project(gemfilter VERSION 0.1.0 LANGUAGES C CXX)

if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
  set(CMAKE_BUILD_TYPE Release CACHE STRING "Build type" FORCE)
  set_property(CACHE CMAKE_BUILD_TYPE PROPERTY STRINGS Debug Release RelWithDebInfo MinSizeRel)
endif()

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_POSITION_INDEPENDENT_CODE ON)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

option(GEMFILTER_WARNINGS_AS_ERRORS "Treat warnings as errors in gemfilter code" OFF)
option(GEMFILTER_BUILD_TESTS "Build unit/integration tests (CTest)" ON)
option(GEMFILTER_WITH_ROOT "Reserved: ROOT-based tools (core never depends on ROOT)" OFF)

include(GNUInstallDirs)
find_package(Threads REQUIRED)

# Warning flags for gemfilter's own targets (third-party code is compiled without them).
if(NOT TARGET gemfilter_warnings)
  add_library(gemfilter_warnings INTERFACE)
  add_library(gemfilter::warnings ALIAS gemfilter_warnings)
  if(CMAKE_CXX_COMPILER_ID MATCHES "Clang|GNU")
    target_compile_options(gemfilter_warnings INTERFACE
      $<$<COMPILE_LANGUAGE:CXX>:-Wall -Wextra -Wpedantic -Wshadow>)
    if(GEMFILTER_WARNINGS_AS_ERRORS)
      target_compile_options(gemfilter_warnings INTERFACE $<$<COMPILE_LANGUAGE:CXX>:-Werror>)
    endif()
  endif()
endif()

# Shared contract headers (include/gemfilter/types.h).  Exposed both as a target and as a
# directory-wide include path so that every track's CMakeLists works without knowing names.
if(NOT EXISTS ${PROJECT_SOURCE_DIR}/include/gemfilter/types.h)
  message(FATAL_ERROR "include/gemfilter/types.h (shared contract header) not found. "
                      "Untar the contract tarball into this tree first.")
endif()
add_library(gemfilter_types INTERFACE)
add_library(gemfilter::types ALIAS gemfilter_types)
target_include_directories(gemfilter_types INTERFACE
  $<BUILD_INTERFACE:${PROJECT_SOURCE_DIR}/include> $<INSTALL_INTERFACE:${CMAKE_INSTALL_INCLUDEDIR}>)
include_directories(${PROJECT_SOURCE_DIR}/include)

if(GEMFILTER_BUILD_TESTS)
  enable_testing()
endif()

# JLab ET + evio (targets gemfilter::et, gemfilter::evio, optionally gemfilter::et_nonet).
add_subdirectory(third_party)

# Track directories, in dependency order; each is optional so partial trees build.
foreach(dir status codec evioio geom seeds track pipeline et apps tools)
  if(EXISTS ${PROJECT_SOURCE_DIR}/${dir}/CMakeLists.txt)
    message(STATUS "gemfilter: adding ${dir}/")
    add_subdirectory(${dir})
  else()
    message(STATUS "gemfilter: ${dir}/ not present, skipped")
  endif()
endforeach()

if(GEMFILTER_BUILD_TESTS)
  file(GLOB _gf_test_dirs LIST_DIRECTORIES true ${PROJECT_SOURCE_DIR}/tests/*)
  foreach(tdir ${_gf_test_dirs})
    if(IS_DIRECTORY ${tdir} AND EXISTS ${tdir}/CMakeLists.txt)
      get_filename_component(_tname ${tdir} NAME)
      message(STATUS "gemfilter: adding tests/${_tname}/")
      add_subdirectory(${tdir} ${CMAKE_BINARY_DIR}/tests/${_tname})
    endif()
  endforeach()
endif()

install(DIRECTORY ${PROJECT_SOURCE_DIR}/include/gemfilter DESTINATION ${CMAKE_INSTALL_INCLUDEDIR})
install(FILES README.md DESTINATION ${CMAKE_INSTALL_DOCDIR} OPTIONAL)
if(EXISTS ${PROJECT_SOURCE_DIR}/docs)
  install(DIRECTORY ${PROJECT_SOURCE_DIR}/docs/ DESTINATION ${CMAKE_INSTALL_DOCDIR}
          FILES_MATCHING PATTERN "*.md")
endif()
