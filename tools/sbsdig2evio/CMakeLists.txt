# sbsdig2evio: libsbsdig digitized tree -> CODA3 EVIO with real MPD/FADC250 banks (SIDIS validation).
# Stage 1 (Python + uproot, no ROOT needed): sbsdig_extract.py  tree -> .sdg
# Stage 2 (C++, this target): sbsdig2evio  .sdg + Podd DB -> .evio + truth sidecar
add_executable(sbsdig2evio sbsdig2evio.cpp)
target_link_libraries(sbsdig2evio PRIVATE gemfilter_apps gemfilter_seeds gemfilter_geom gemfilter_codec gemfilter_evioio)
add_executable(sbsdig2evio_verify sbsdig2evio_verify.cpp)
target_link_libraries(sbsdig2evio_verify PRIVATE gemfilter_apps gemfilter_seeds gemfilter_geom gemfilter_codec gemfilter_evioio)
install(TARGETS sbsdig2evio sbsdig2evio_verify RUNTIME DESTINATION bin)
install(PROGRAMS sbsdig_extract.py make_synthetic_sbsdig_tree.py make_sim_db.py DESTINATION bin)
install(FILES libsbsdig_gem_sigflag.patch DESTINATION share/gemfilter)
