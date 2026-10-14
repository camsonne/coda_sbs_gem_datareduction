# gemfilter_seeds : FADC250 / VTP cluster decoding, calorimeter clustering, constraint roads.
cmake_minimum_required(VERSION 3.16)
add_library(gemfilter_seeds src/Fadc250.cxx src/VtpCluster.cxx src/CaloClustering.cxx src/Roads.cxx)
add_library(gemfilter::seeds ALIAS gemfilter_seeds)
target_compile_features(gemfilter_seeds PUBLIC cxx_std_17)
set_target_properties(gemfilter_seeds PROPERTIES CXX_EXTENSIONS OFF POSITION_INDEPENDENT_CODE ON)
target_include_directories(gemfilter_seeds PUBLIC
  $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include> $<INSTALL_INTERFACE:include>)
target_link_libraries(gemfilter_seeds PUBLIC gemfilter_geom)
if(CMAKE_CXX_COMPILER_ID MATCHES "Clang|GNU")
  target_compile_options(gemfilter_seeds PRIVATE -Wall -Wextra -Wpedantic)
endif()
install(TARGETS gemfilter_seeds ARCHIVE DESTINATION lib LIBRARY DESTINATION lib)
install(DIRECTORY include/gemfilter/seeds DESTINATION include/gemfilter)
