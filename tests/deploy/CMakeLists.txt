# CODA deployment kit tests.  Both install the build tree into a scratch prefix first.
#   deploy.kit_static : script syntax, config rendering + DB load for both arms, error paths,
#                       gemfilter_check without a running ET (no sockets needed)
#   deploy.kit_live   : (GEMFILTER_LIVE_ET_TEST=ON) full run-control cycle against a real ET
#                       system: prestart hook starts BB+SBS stations upstream of a stand-in ER,
#                       events are fed and drained, end/reset hooks, misplacement recovery.
find_program(GF_BASH bash REQUIRED)
add_test(NAME deploy.kit_static
         COMMAND ${GF_BASH} ${CMAKE_CURRENT_SOURCE_DIR}/kit_static_test.sh ${CMAKE_BINARY_DIR}
                 ${PROJECT_SOURCE_DIR} ${PROJECT_SOURCE_DIR}/tests/seeds/data/DB)
set_tests_properties(deploy.kit_static PROPERTIES TIMEOUT 120)
if(GEMFILTER_LIVE_ET_TEST AND TARGET gf_et_status)
  add_test(NAME deploy.kit_live
           COMMAND ${GF_BASH} ${CMAKE_CURRENT_SOURCE_DIR}/kit_live_test.sh ${CMAKE_BINARY_DIR}
                   ${PROJECT_SOURCE_DIR} ${PROJECT_SOURCE_DIR}/tests/seeds/data/DB)
  set_tests_properties(deploy.kit_live PROPERTIES TIMEOUT 400)
endif()
