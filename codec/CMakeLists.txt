# gemfilter_codec: SSP/VTP MPD GEM data decoder + bit-exact re-encoder (GEM codec track)
cmake_minimum_required(VERSION 3.16)
if(NOT DEFINED GEMFILTER_ROOT)
  get_filename_component(GEMFILTER_ROOT "${CMAKE_CURRENT_LIST_DIR}/.." ABSOLUTE)
endif()

add_library(gemfilter_codec
  src/gem_codec.cpp
  src/synthetic.cpp)
add_library(gemfilter::codec ALIAS gemfilter_codec)
target_include_directories(gemfilter_codec PUBLIC
  $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
  $<BUILD_INTERFACE:${GEMFILTER_ROOT}/include>)
target_compile_features(gemfilter_codec PUBLIC cxx_std_17)
set_target_properties(gemfilter_codec PROPERTIES CXX_EXTENSIONS OFF POSITION_INDEPENDENT_CODE ON)
if(CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang")
  target_compile_options(gemfilter_codec PRIVATE -Wall -Wextra -Wpedantic)
endif()
