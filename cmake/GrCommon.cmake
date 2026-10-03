# Settings shared by every C++ project in the repo. Include before the first target.

if(NOT WIN32)
    message(FATAL_ERROR "Garry's Redemption is Windows only.")
endif()
if(NOT CMAKE_SIZEOF_VOID_P EQUAL 8)
    message(FATAL_ERROR "Garry's Redemption is 64-bit only. Configure for x64.")
endif()

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

# Static CRT: the .asi and the GMod module must load on a machine that has no Visual C++
# redistributable installed.
set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")

get_filename_component(GR_REPO_ROOT "${CMAKE_CURRENT_LIST_DIR}/.." ABSOLUTE)

add_library(gr_common INTERFACE)
target_include_directories(gr_common INTERFACE
    "${GR_REPO_ROOT}/protocol"
    "${GR_REPO_ROOT}/common")
target_compile_definitions(gr_common INTERFACE
    WIN32_LEAN_AND_MEAN NOMINMAX _CRT_SECURE_NO_WARNINGS)
if(MSVC)
    target_compile_options(gr_common INTERFACE /W4 /WX /permissive- /utf-8)
endif()
