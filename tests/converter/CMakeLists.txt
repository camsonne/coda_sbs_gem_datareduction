# converter.roundtrip: sbsdig2evio on a SYNTHETIC tree with the libsbsdig mcpass4 branch layout.
# Needs python3 with uproot + awkward; reported as SKIPPED (exit 77) otherwise.
find_package(Python3 COMPONENTS Interpreter)
if(Python3_Interpreter_FOUND AND TARGET sbsdig2evio AND TARGET gemfilter_file)
  add_test(NAME converter.roundtrip
           COMMAND ${CMAKE_CURRENT_SOURCE_DIR}/run_roundtrip.sh ${Python3_EXECUTABLE}
                   ${PROJECT_SOURCE_DIR}/tools/sbsdig2evio $<TARGET_FILE_DIR:sbsdig2evio> $<TARGET_FILE:gemfilter_file>
                   ${PROJECT_SOURCE_DIR}/tests/seeds/data/DB
                   ${CMAKE_CURRENT_SOURCE_DIR}/data/db_gen_conf_8gemmodules_GEN2.dat
                   ${CMAKE_CURRENT_BINARY_DIR}/work)
  set_tests_properties(converter.roundtrip PROPERTIES SKIP_RETURN_CODE 77 TIMEOUT 300)
else()
  message(STATUS "gemfilter: converter.roundtrip test disabled (no python3 or targets)")
endif()
