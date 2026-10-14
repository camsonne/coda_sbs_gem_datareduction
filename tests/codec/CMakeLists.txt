# Tests for the GEM codec track (codec + evioio). Registered with CTest.
cmake_minimum_required(VERSION 3.16)

add_executable(test_codec test_codec.cpp)
target_link_libraries(test_codec PRIVATE gemfilter_codec)
add_test(NAME codec.unit COMMAND test_codec)
add_executable(test_evioio test_evioio.cpp)
target_link_libraries(test_evioio PRIVATE gemfilter_evioio gemfilter_codec)
add_test(NAME evioio.unit COMMAND test_evioio)
