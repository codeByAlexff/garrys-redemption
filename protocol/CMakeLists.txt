cmake_minimum_required(VERSION 3.21)
project(gr_protocol LANGUAGES CXX)

include("${CMAKE_CURRENT_LIST_DIR}/../cmake/GrCommon.cmake")
enable_testing()

# The protocol itself is header-only. This project builds its unit tests and gr_probe,
# a console stand-in for either side that runs the real C++ link code.

add_executable(gr_tests
    tests/main.cpp
    tests/test_units.cpp
    tests/test_seqlock.cpp
    tests/test_link.cpp
    tests/test_log.cpp
    tests/test_keep_active.cpp
    tests/test_guest.cpp)
target_link_libraries(gr_tests PRIVATE gr_common)
# keep_active.h and guest.h belong to the GMod module but need none of GMod's headers, so they are
# tested here, where the tests build on a machine that has not fetched them.
target_include_directories(gr_tests PRIVATE "${GR_REPO_ROOT}/gmod/module/src")
add_test(NAME gr_tests COMMAND gr_tests)

add_executable(gr_probe "${GR_REPO_ROOT}/tools/gr_probe/gr_probe.cpp")
target_link_libraries(gr_probe PRIVATE gr_common)

# What the monitor really shows (desktop duplication), for looking at the games from scripts.
add_executable(gr_screen "${GR_REPO_ROOT}/tools/gr_screen/gr_screen.cpp")
target_link_libraries(gr_screen PRIVATE d3d11 dxgi)

# tools/gr_layout.py reads gr_protocol.h instead of keeping a copy of the structs. This
# turns what it read into static_asserts, so if the parser and the compiler ever disagree
# about an offset, a size or a constant, the build fails here.
find_package(Python3 COMPONENTS Interpreter)
if(Python3_FOUND)
    set(layout_check "${CMAKE_CURRENT_BINARY_DIR}/layout_check.cpp")
    add_custom_command(
        OUTPUT "${layout_check}"
        COMMAND Python3::Interpreter "${GR_REPO_ROOT}/tools/gr_layout.py"
                --emit-check "${layout_check}"
        DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/gr_protocol.h" "${GR_REPO_ROOT}/tools/gr_layout.py"
        COMMENT "Generating layout_check.cpp from gr_protocol.h"
        VERBATIM)
    add_library(gr_layout_check OBJECT "${layout_check}")
    target_link_libraries(gr_layout_check PRIVATE gr_common)
    add_dependencies(gr_tests gr_layout_check)
else()
    message(WARNING "Python 3 not found: the Python layout parser will not be checked against the compiler.")
endif()
