cmake_minimum_required(VERSION 3.21)
project(gr_gmod_module LANGUAGES CXX)

include("${CMAKE_CURRENT_LIST_DIR}/../../cmake/GrCommon.cmake")

# The GMod side: a clientside binary module. GMod finds it by file name, so the name is
# fixed: gmcl_ (client), gr (what Lua passes to require), _win64.

# Facepunch's module headers are fetched per machine and not committed.
set(GR_GMOD_MODULE_BASE "${CMAKE_CURRENT_SOURCE_DIR}/third_party/gmod-module-base"
    CACHE PATH "Checkout of github.com/Facepunch/gmod-module-base (development branch)")
if(NOT EXISTS "${GR_GMOD_MODULE_BASE}/include/GarrysMod/Lua/Interface.h")
    message(FATAL_ERROR
        "gmod-module-base headers not found in ${GR_GMOD_MODULE_BASE}.\n"
        "Get them with:\n"
        "  git clone --depth 1 --branch development https://github.com/Facepunch/gmod-module-base \"${GR_GMOD_MODULE_BASE}\"")
endif()

add_library(gmcl_gr_win64 MODULE src/module.cpp)
set_target_properties(gmcl_gr_win64 PROPERTIES PREFIX "" OUTPUT_NAME "gmcl_gr_win64")
# SYSTEM: the project builds with /W4 /WX, which is not ours to impose on their headers.
target_include_directories(gmcl_gr_win64 SYSTEM PRIVATE "${GR_GMOD_MODULE_BASE}/include")
target_link_libraries(gmcl_gr_win64 PRIVATE gr_common d3d9 d3d11 dxgi dcomp d3dcompiler)

# The same DLL again under the server-realm name: GMod only lets the server realm require
# gmsv_ files. That copy hands its Lua state to the gmcl_ image (src/module.cpp), so both
# realms share one link.
add_custom_command(TARGET gmcl_gr_win64 POST_BUILD
    COMMAND "${CMAKE_COMMAND}" -E copy_if_different "$<TARGET_FILE:gmcl_gr_win64>"
            "$<TARGET_FILE_DIR:gmcl_gr_win64>/gmsv_gr_win64.dll"
    VERBATIM)
