# et/: ET filter-station engine, EVIO buffer framing, and the libet backend.
#   gemfilter_et_core  framing + engine (no ET dependency; testable with a fake backend)
#   gemfilter_et       core + LibEtBackend linked against JLab ET (gemfilter::et)
#   gemfilter_et_nonet (test-only) same backend linked against the et_nonet ET variant
add_library(gemfilter_et_core src/evio_framing.cpp src/et_filter.cpp)
add_library(gemfilter::et_core ALIAS gemfilter_et_core)
target_include_directories(gemfilter_et_core PUBLIC
  $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
  $<INSTALL_INTERFACE:${CMAKE_INSTALL_INCLUDEDIR}>)
target_link_libraries(gemfilter_et_core PUBLIC gemfilter::status Threads::Threads
                      PRIVATE gemfilter::warnings)

add_library(gemfilter_et src/libet_backend.cpp)
add_library(gemfilter::et_station ALIAS gemfilter_et)
target_link_libraries(gemfilter_et PUBLIC gemfilter_et_core PRIVATE gemfilter::et gemfilter::warnings)

if(GEMFILTER_HAVE_ET_NONET)
  add_library(gemfilter_et_nonet STATIC EXCLUDE_FROM_ALL src/libet_backend.cpp)
  target_link_libraries(gemfilter_et_nonet PUBLIC gemfilter_et_core gemfilter::et_nonet
                        PRIVATE gemfilter::warnings)
endif()

install(TARGETS gemfilter_et_core gemfilter_et ARCHIVE DESTINATION ${CMAKE_INSTALL_LIBDIR}
        LIBRARY DESTINATION ${CMAKE_INSTALL_LIBDIR})
install(DIRECTORY include/gemfilter DESTINATION ${CMAKE_INSTALL_INCLUDEDIR})
