# gemfilter_pipeline : the production EventProcessor (codec + seeds + track + evioio).
cmake_minimum_required(VERSION 3.16)
add_library(gemfilter_pipeline src/GemFilterProcessor.cxx)
add_library(gemfilter::pipeline ALIAS gemfilter_pipeline)
target_compile_features(gemfilter_pipeline PUBLIC cxx_std_17)
set_target_properties(gemfilter_pipeline PROPERTIES CXX_EXTENSIONS OFF POSITION_INDEPENDENT_CODE ON)
target_include_directories(gemfilter_pipeline PUBLIC
  $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include> $<INSTALL_INTERFACE:include>)
target_link_libraries(gemfilter_pipeline PUBLIC gemfilter_track gemfilter_seeds gemfilter_geom gemfilter_codec gemfilter_evioio)
if(CMAKE_CXX_COMPILER_ID MATCHES "Clang|GNU")
  target_compile_options(gemfilter_pipeline PRIVATE -Wall -Wextra -Wpedantic)
endif()
install(TARGETS gemfilter_pipeline ARCHIVE DESTINATION lib LIBRARY DESTINATION lib)
install(DIRECTORY include/gemfilter DESTINATION include)
