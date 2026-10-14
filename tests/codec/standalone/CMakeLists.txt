# Standalone build of the GEM codec track (codec/, evioio/, tests/codec/) without the
# CODA-integration top level. Usage:
#   cmake -S tests/codec/standalone -B build-codec -G Ninja [-DGEMFILTER_EVIO_SOURCE_DIR=/path/to/evio-main]
#   cmake --build build-codec && ctest --test-dir build-codec --output-on-failure
cmake_minimum_required(VERSION 3.16)
project(gemfilter_codec_standalone LANGUAGES C CXX)
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
  set(CMAKE_BUILD_TYPE RelWithDebInfo)
endif()
get_filename_component(GEMFILTER_ROOT "${CMAKE_CURRENT_LIST_DIR}/../../.." ABSOLUTE)
enable_testing()
add_subdirectory(${GEMFILTER_ROOT}/codec  ${CMAKE_BINARY_DIR}/codec)
if(EXISTS ${GEMFILTER_ROOT}/evioio/CMakeLists.txt)
  add_subdirectory(${GEMFILTER_ROOT}/evioio ${CMAKE_BINARY_DIR}/evioio)
endif()
add_subdirectory(${GEMFILTER_ROOT}/tests/codec ${CMAKE_BINARY_DIR}/tests_codec)
