cmake_minimum_required(VERSION 3.21)
project(gr_rdr2 LANGUAGES CXX)

include("${CMAKE_CURRENT_LIST_DIR}/../cmake/GrCommon.cmake")

# The RDR2 side: an ASI plugin (a DLL with another extension) that Script Hook RDR2 runs
# on the game's script thread.

add_library(GarrysRedemption MODULE
    src/dllmain.cpp
    src/plugin.cpp)
set_target_properties(GarrysRedemption PROPERTIES PREFIX "" SUFFIX ".asi")
target_link_libraries(GarrysRedemption PRIVATE gr_common)

# ScriptHookRDR2.dll's import library. The SDK's own is used when it has been unpacked to
# rdr2/sdk (never committed: its licence forbids redistribution). Without the SDK, one is
# generated from scripthook_imports.def, which lists the same six functions.
set(GR_SCRIPTHOOK_SDK_LIB "${CMAKE_CURRENT_SOURCE_DIR}/sdk/lib/ScriptHookRDR2.lib")
if(EXISTS "${GR_SCRIPTHOOK_SDK_LIB}")
    message(STATUS "Script Hook RDR2: linking the SDK import library in rdr2/sdk")
    target_link_libraries(GarrysRedemption PRIVATE "${GR_SCRIPTHOOK_SDK_LIB}")
else()
    message(STATUS "Script Hook RDR2: no SDK in rdr2/sdk, generating the import library from scripthook_imports.def")
    set(scripthook_def "${CMAKE_CURRENT_SOURCE_DIR}/scripthook_imports.def")
    set(scripthook_lib "${CMAKE_CURRENT_BINARY_DIR}/ScriptHookRDR2.lib")
    add_custom_command(
        OUTPUT "${scripthook_lib}"
        COMMAND "${CMAKE_AR}" /nologo "/def:${scripthook_def}" "/out:${scripthook_lib}" /machine:x64
        DEPENDS "${scripthook_def}"
        COMMENT "Generating ScriptHookRDR2.lib from scripthook_imports.def"
        VERBATIM)
    add_custom_target(gr_scripthook_implib DEPENDS "${scripthook_lib}")
    add_dependencies(GarrysRedemption gr_scripthook_implib)
    target_link_libraries(GarrysRedemption PRIVATE "${scripthook_lib}")
endif()

# Test harness: runs the built .asi without the game. gr_fake_scripthook is a stand-in for
# ScriptHookRDR2.dll written from scripthook.h (nothing of the real one is in it), and
# asi_runner plays the game's part. Both go to build/harness, apart from the .asi, so a
# file called ScriptHookRDR2.dll never sits next to what gets packaged.
set(harness_src "${GR_REPO_ROOT}/tools/fake_scripthook")
set(harness_out "${CMAKE_CURRENT_BINARY_DIR}/harness/$<CONFIG>")

# Every hash in native_hashes.h, as a list the fake Script Hook includes: a native the
# plugin has recorded there is answered with zero, and only one that is not counts as unknown.
set(native_hashes "${CMAKE_CURRENT_SOURCE_DIR}/src/native_hashes.h")
set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS "${native_hashes}")
file(STRINGS "${native_hashes}" hash_lines REGEX "constexpr uint64_t [A-Z0-9_]+ = 0x[0-9A-Fa-f]+")
set(known_hashes "")
foreach(line IN LISTS hash_lines)
    string(REGEX MATCH "0x[0-9A-Fa-f]+" value "${line}")
    string(APPEND known_hashes "${value}ull,
")
endforeach()
file(WRITE "${CMAKE_CURRENT_BINARY_DIR}/generated/known_native_hashes.inc" "${known_hashes}")

add_library(gr_fake_scripthook SHARED "${harness_src}/fake_scripthook.cpp")
target_include_directories(gr_fake_scripthook PRIVATE "${CMAKE_CURRENT_BINARY_DIR}/generated")
set_target_properties(gr_fake_scripthook PROPERTIES PREFIX "" OUTPUT_NAME "ScriptHookRDR2"
    RUNTIME_OUTPUT_DIRECTORY "${harness_out}" ARCHIVE_OUTPUT_DIRECTORY "${harness_out}")
target_include_directories(gr_fake_scripthook PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/src")
target_link_libraries(gr_fake_scripthook PRIVATE gr_common)

add_executable(asi_runner "${harness_src}/asi_runner.cpp")
set_target_properties(asi_runner PROPERTIES RUNTIME_OUTPUT_DIRECTORY "${harness_out}")
target_link_libraries(asi_runner PRIVATE gr_common gr_fake_scripthook)
add_dependencies(asi_runner GarrysRedemption)
