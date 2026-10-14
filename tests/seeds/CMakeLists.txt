# Tests for geom + seeds, run against the SBS-replay DB snapshot in tests/seeds/data/DB.
add_executable(test_seeds test_seeds.cpp)
target_include_directories(test_seeds PRIVATE ${PROJECT_SOURCE_DIR}/tests/codec)
target_link_libraries(test_seeds PRIVATE gemfilter_seeds)
target_compile_definitions(test_seeds PRIVATE GF_TEST_DB="${CMAKE_CURRENT_SOURCE_DIR}/data/DB")
add_test(NAME seeds.unit COMMAND test_seeds)
