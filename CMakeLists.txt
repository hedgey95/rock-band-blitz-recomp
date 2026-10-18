# blitz - ReXGlue Recompiled Project
#
# SDK boilerplate lives in generated/rexglue.cmake.

cmake_minimum_required(VERSION 3.25)
project(blitz LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

include(generated/rexglue.cmake)

# Sources
set(REXGLUE_SDK_SOURCE_DIR "${CMAKE_CURRENT_SOURCE_DIR}/sdk")

set(BLITZ_SOURCES
    src/main.cpp
    src/input_record.cpp
    src/input_replay.cpp
    src/input_tape.cpp
    src/diagnostics.cpp
    src/title_keys.cpp
    src/content_install.cpp
    src/hub.cpp
    src/custom_song_checks.cpp
    src/renderer/native_graphics.cpp
    src/renderer/d3d_hooks.cpp
    src/renderer/d3d_ops.cpp
    src/renderer/device_trace.cpp
    src/renderer/shader_capture.cpp
    src/renderer/texture_decode.cpp
    src/renderer/texture_dump.cpp
    src/renderer/texture_cache.cpp
    src/renderer/shader_container.cpp
    src/renderer/vertex_input.cpp
    src/renderer/renderer.cpp
    src/renderer/shader_translate.cpp
    src/renderer/renderer_draw.cpp
    # Xenos texture layout math from the SDK (BSD, derived from Xenia), compiled
    # in because the GPU plugin it ships in is not part of the native path.
    ${REXGLUE_SDK_SOURCE_DIR}/src/graphics/pipeline/texture/util.cpp
    ${REXGLUE_SDK_SOURCE_DIR}/src/graphics/pipeline/texture/info.cpp
    ${REXGLUE_SDK_SOURCE_DIR}/src/graphics/pipeline/texture/info_formats.cpp
)

# The native renderer's own shaders, compiled with dxc from the Windows SDK.
file(GLOB _blitz_dxc "$ENV{ProgramFiles\(x86\)}/Windows Kits/10/bin/10.*/x64/dxc.exe")
list(SORT _blitz_dxc)
list(GET _blitz_dxc -1 BLITZ_DXC)
set(BLITZ_SHADER_HEADERS "")
foreach(_shader present_cs resolve_depth_cs resolve_depth_ms_cs)
    set(_out "${CMAKE_CURRENT_BINARY_DIR}/shaders/${_shader}.h")
    add_custom_command(OUTPUT "${_out}"
        COMMAND ${CMAKE_COMMAND} -E make_directory "${CMAKE_CURRENT_BINARY_DIR}/shaders"
        COMMAND "${BLITZ_DXC}" -nologo -T cs_6_0 -E main -Vn g_${_shader} -Fh "${_out}"
                "${CMAKE_CURRENT_SOURCE_DIR}/src/renderer/shaders/${_shader}.hlsl"
        DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/src/renderer/shaders/${_shader}.hlsl"
        COMMENT "dxc ${_shader}")
    list(APPEND BLITZ_SHADER_HEADERS "${_out}")
endforeach()
list(APPEND BLITZ_SOURCES ${BLITZ_SHADER_HEADERS})

if(WIN32)
    add_executable(blitz WIN32 ${BLITZ_SOURCES})
else()
    add_executable(blitz ${BLITZ_SOURCES})
endif()

rexglue_setup_target(blitz)
target_link_libraries(blitz PRIVATE dbghelp)
target_include_directories(blitz PRIVATE "${CMAKE_CURRENT_BINARY_DIR}")  # shaders/*.h
# Shaders missing from --native_shader_dir are translated inside the game
# (src/renderer/shader_translate.cpp): XenosRecomp's translator is built in,
# with shader_common.h embedded, and dxc runs through dxcompiler.dll.
set(XENOS_RECOMP_DIR "${CMAKE_CURRENT_SOURCE_DIR}/thirdparty/XenosRecomp")
target_sources(blitz PRIVATE "${XENOS_RECOMP_DIR}/XenosRecomp/shader_recompiler.cpp")
set_source_files_properties("${XENOS_RECOMP_DIR}/XenosRecomp/shader_recompiler.cpp" PROPERTIES
    COMPILE_OPTIONS "-include;windows.h;-include;objbase.h;-include;${XENOS_RECOMP_DIR}/XenosRecomp/pch.h;-Wno-switch;-Wno-unused-variable;-Wno-null-arithmetic;-fms-extensions;-w")
target_compile_definitions(blitz PRIVATE BLITZ_RECOMP _CRT_SECURE_NO_WARNINGS)
# pch.h includes these headers; only fmt is used, and that comes from the SDK.
target_include_directories(blitz PRIVATE
    "${XENOS_RECOMP_DIR}/XenosRecomp"
    "${XENOS_RECOMP_DIR}/thirdparty/smol-v/source"
    "${XENOS_RECOMP_DIR}/thirdparty/xxHash"
    "${XENOS_RECOMP_DIR}/thirdparty/zstd/lib")
set(_shader_common "${XENOS_RECOMP_DIR}/XenosRecomp/shader_common.h")
set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS "${_shader_common}")
file(READ "${_shader_common}" _bytes HEX)
string(REGEX REPLACE "([0-9a-f][0-9a-f])" "0x\\1," _bytes "${_bytes}")
file(CONFIGURE OUTPUT "${CMAKE_CURRENT_BINARY_DIR}/shader_common_bytes.h"
    CONTENT "// Generated from XenosRecomp's shader_common.h (CMakeLists.txt).\ninline constexpr unsigned char kShaderCommon[] = {${_bytes}};\n")
get_filename_component(_sdk_bin "${BLITZ_DXC}" DIRECTORY)        # .../bin/<version>/x64
get_filename_component(_sdk_version "${_sdk_bin}" DIRECTORY)
get_filename_component(_sdk_version "${_sdk_version}" NAME)
target_link_libraries(blitz PRIVATE
    "$ENV{ProgramFiles\(x86\)}/Windows Kits/10/Lib/${_sdk_version}/um/x64/dxcompiler.lib")
add_custom_command(TARGET blitz POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E copy_if_different "${_sdk_bin}/dxcompiler.dll" "${_sdk_bin}/dxil.dll"
            "$<TARGET_FILE_DIR:blitz>")

# Stock Xenia GPU plugin, kept only as the reference renderer while the native
# one is built. Loaded at runtime with --gpu_plugin=xenos.
add_custom_command(TARGET blitz POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E copy_if_different $<TARGET_FILE:rex::gpu-xenos> $<TARGET_FILE_DIR:blitz>)

# Plain self-checks for logic that has no SDK dependencies.
add_executable(blitz_tests tests/input_tape_test.cpp src/input_tape.cpp
    tests/vertex_input_test.cpp src/renderer/vertex_input.cpp src/renderer/shader_container.cpp)
target_include_directories(blitz_tests PRIVATE ${REXGLUE_SDK_SOURCE_DIR}/include)
target_compile_definitions(blitz_tests PRIVATE NOMINMAX)
