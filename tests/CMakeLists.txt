cmake_minimum_required(VERSION 3.28)
project(casparcg_consumer_tests LANGUAGES CXX)

find_package(Boost 1.83 CONFIG REQUIRED COMPONENTS log thread locale)
find_package(Threads REQUIRED)
enable_testing()

add_executable(consumer_frame_delay_test
    consumer_frame_delay_test.cpp
    ../src/core/frame/frame.cpp
    ../src/core/frame/geometry.cpp
    ../src/common/utf.cpp
)
target_compile_features(consumer_frame_delay_test PRIVATE cxx_std_20)
target_compile_definitions(consumer_frame_delay_test PRIVATE BOOST_ALL_DYN_LINK)
target_include_directories(consumer_frame_delay_test PRIVATE ../src)
target_precompile_headers(consumer_frame_delay_test PRIVATE <boost/algorithm/string.hpp>)
target_link_libraries(consumer_frame_delay_test PRIVATE Boost::log Boost::thread Boost::locale Threads::Threads ${CMAKE_DL_LIBS})
add_test(NAME consumer_frame_delay COMMAND consumer_frame_delay_test)
set_tests_properties(consumer_frame_delay PROPERTIES TIMEOUT 15)
