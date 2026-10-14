# gemfilter_status: GEMFILTER status bank, CODA3 event classification, FilterPolicy
# (always-pass list, prescaled raw pass-through, error policy) and trivial processors.
add_library(gemfilter_status
  src/coda_event.cpp
  src/status_bank.cpp
  src/policy.cpp
  src/processors.cpp)
add_library(gemfilter::status ALIAS gemfilter_status)
target_include_directories(gemfilter_status PUBLIC
  $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
  $<INSTALL_INTERFACE:${CMAKE_INSTALL_INCLUDEDIR}>)
target_link_libraries(gemfilter_status PUBLIC gemfilter::types PRIVATE gemfilter::warnings)

install(TARGETS gemfilter_status ARCHIVE DESTINATION ${CMAKE_INSTALL_LIBDIR}
        LIBRARY DESTINATION ${CMAKE_INSTALL_LIBDIR})
install(DIRECTORY include/gemfilter DESTINATION ${CMAKE_INSTALL_INCLUDEDIR})
