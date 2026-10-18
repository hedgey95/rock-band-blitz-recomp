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

# Stock Xenia GPU plugin, kept only as the reference renderer while the native
# one is built. Loaded at runtime with --gpu_plugin=xenos.
add_custom_command(TARGET blitz POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E copy_if_different $<TARGET_FILE:rex::gpu-xenos> $<TARGET_FILE_DIR:blitz>)

# Plain self-checks for logic that has no SDK dependencies.
add_executable(blitz_tests tests/input_tape_test.cpp src/input_tape.cpp
    tests/vertex_input_test.cpp src/renderer/vertex_input.cpp src/renderer/shader_container.cpp)
target_include_directories(blitz_tests PRIVATE ${REXGLUE_SDK_SOURCE_DIR}/include)
target_compile_definitions(blitz_tests PRIVATE NOMINMAX)
