# gemfilter_geom : Podd DB reader, GEM / calorimeter geometry, APV maps, arm configs.
# Owned by the Seeds & geometry track.
cmake_minimum_required(VERSION 3.16)

add_library(gemfilter_geom
  src/PoddDb.cxx
  src/GemGeometry.cxx
  src/CaloGeometry.cxx
  src/ArmConfig.cxx)
add_library(gemfilter::geom ALIAS gemfilter_geom)
target_compile_features(gemfilter_geom PUBLIC cxx_std_17)
set_target_properties(gemfilter_geom PROPERTIES CXX_EXTENSIONS OFF POSITION_INDEPENDENT_CODE ON)
# own headers + the shared contract header include/gemfilter/types.h at the gemfilter root
target_include_directories(gemfilter_geom PUBLIC
  $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
  $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/../include>
  $<INSTALL_INTERFACE:include>)
if(CMAKE_CXX_COMPILER_ID MATCHES "Clang|GNU")
  target_compile_options(gemfilter_geom PRIVATE -Wall -Wextra -Wpedantic)
endif()
install(TARGETS gemfilter_geom ARCHIVE DESTINATION lib LIBRARY DESTINATION lib)
install(DIRECTORY include/gemfilter/geom DESTINATION include/gemfilter)
