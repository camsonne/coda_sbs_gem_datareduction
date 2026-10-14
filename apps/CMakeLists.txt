# apps/: file-mode driver library + executables gemfilter_file and gemfilter_et.
add_library(gemfilter_apps src/event_io.cpp src/file_driver.cpp src/processor_registry.cpp)
add_library(gemfilter::apps ALIAS gemfilter_apps)
target_include_directories(gemfilter_apps PUBLIC
  $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
  $<INSTALL_INTERFACE:${CMAKE_INSTALL_INCLUDEDIR}>)
target_link_libraries(gemfilter_apps PUBLIC gemfilter::status PRIVATE gemfilter::evio gemfilter::warnings)

# Hook for the real pipeline (codec + seeds + track), see processor_registry.h.
if(TARGET gemfilter_pipeline)
  target_link_libraries(gemfilter_apps PUBLIC gemfilter_pipeline)
  target_compile_definitions(gemfilter_apps PRIVATE GEMFILTER_HAVE_PIPELINE=1)
  message(STATUS "gemfilter: apps linked with gemfilter_pipeline")
endif()

add_executable(gemfilter_file src/gemfilter_file_main.cpp)
target_link_libraries(gemfilter_file PRIVATE gemfilter_apps gemfilter::warnings)

if(TARGET gemfilter_et)
  # Target name differs from the library gemfilter_et; the binary is still "gemfilter_et".
  add_executable(gemfilter_et_daemon src/gemfilter_et_main.cpp)
  set_target_properties(gemfilter_et_daemon PROPERTIES OUTPUT_NAME gemfilter_et)
  target_link_libraries(gemfilter_et_daemon PRIVATE gemfilter_apps gemfilter_et gemfilter::warnings)
  install(TARGETS gemfilter_et_daemon RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR})
endif()

set_target_properties(gemfilter_file PROPERTIES RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/bin)
if(TARGET gemfilter_et_daemon)
  set_target_properties(gemfilter_et_daemon PROPERTIES RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/bin)
endif()

install(TARGETS gemfilter_apps gemfilter_file
        RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR}
        ARCHIVE DESTINATION ${CMAKE_INSTALL_LIBDIR} LIBRARY DESTINATION ${CMAKE_INSTALL_LIBDIR})
install(DIRECTORY include/gemfilter DESTINATION ${CMAKE_INSTALL_INCLUDEDIR})
