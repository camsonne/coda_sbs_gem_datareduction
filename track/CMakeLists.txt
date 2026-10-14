# gemfilter_track : strip clustering, 2D hits, road tracking, keep masks; SYNTHETIC toy generator.
cmake_minimum_required(VERSION 3.16)
add_library(gemfilter_track src/GemEventFilter.cxx src/Toy.cxx)
add_library(gemfilter::track ALIAS gemfilter_track)
target_compile_features(gemfilter_track PUBLIC cxx_std_17)
set_target_properties(gemfilter_track PROPERTIES CXX_EXTENSIONS OFF POSITION_INDEPENDENT_CODE ON)
target_include_directories(gemfilter_track PUBLIC
  $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include> $<INSTALL_INTERFACE:include>)
target_link_libraries(gemfilter_track PUBLIC gemfilter_seeds gemfilter_geom)
if(CMAKE_CXX_COMPILER_ID MATCHES "Clang|GNU")
  target_compile_options(gemfilter_track PRIVATE -Wall -Wextra -Wpedantic)
endif()
install(TARGETS gemfilter_track ARCHIVE DESTINATION lib LIBRARY DESTINATION lib)
install(DIRECTORY include/gemfilter/track DESTINATION include/gemfilter)
