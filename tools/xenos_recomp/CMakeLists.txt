# Builds thirdparty/XenosRecomp (pinned submodule, plus patches/xenos-recomp.diff)
# with the DirectX Shader Compiler from the installed Windows SDK, so the
# submodule's prebuilt dxc-bin binaries are not needed.
#
#   cmake -S tools/xenos_recomp -B out/build/xenos_recomp -G Ninja -DCMAKE_BUILD_TYPE=Release
#   cmake --build out/build/xenos_recomp
cmake_minimum_required(VERSION 3.20)
project(blitz_xenos_recomp C CXX)
set(CMAKE_CXX_STANDARD 17)

set(XENOS_RECOMP_ROOT "${CMAKE_CURRENT_SOURCE_DIR}/../../thirdparty/XenosRecomp")
set(XENOS_RECOMP_THIRDPARTY_ROOT "${XENOS_RECOMP_ROOT}/thirdparty")

# DXC from the Windows SDK: dxcapi.h, dxcompiler.lib, dxcompiler.dll and dxil.dll
# (dxcompiler loads dxil.dll to validate and sign DXIL).
set(WINSDK_VERSION "${CMAKE_SYSTEM_VERSION}")
if(NOT WINSDK_VERSION OR NOT EXISTS "$ENV{ProgramFiles\(x86\)}/Windows Kits/10/bin/${WINSDK_VERSION}/x64/dxcompiler.dll")
  file(GLOB _sdk_bins "$ENV{ProgramFiles\(x86\)}/Windows Kits/10/bin/10.*/x64/dxcompiler.dll")
  list(SORT _sdk_bins)
  list(GET _sdk_bins -1 _dxc_dll)
  get_filename_component(_bin_dir "${_dxc_dll}" DIRECTORY)
  get_filename_component(_ver_dir "${_bin_dir}" DIRECTORY)
  get_filename_component(WINSDK_VERSION "${_ver_dir}" NAME)
endif()
set(WINSDK_ROOT "$ENV{ProgramFiles\(x86\)}/Windows Kits/10")
message(STATUS "DXC from Windows SDK ${WINSDK_VERSION}")

add_library(Microsoft::DirectXShaderCompiler SHARED IMPORTED)
set_target_properties(Microsoft::DirectXShaderCompiler PROPERTIES
  IMPORTED_IMPLIB "${WINSDK_ROOT}/Lib/${WINSDK_VERSION}/um/x64/dxcompiler.lib"
  IMPORTED_LOCATION "${WINSDK_ROOT}/bin/${WINSDK_VERSION}/x64/dxcompiler.dll"
  INTERFACE_INCLUDE_DIRECTORIES "${WINSDK_ROOT}/Include/${WINSDK_VERSION}/um")
add_library(Microsoft::DXIL INTERFACE IMPORTED)
set(DXIL_DLL "${WINSDK_ROOT}/bin/${WINSDK_VERSION}/x64/dxil.dll")

add_subdirectory("${XENOS_RECOMP_THIRDPARTY_ROOT}" thirdparty)
add_subdirectory("${XENOS_RECOMP_ROOT}/XenosRecomp" XenosRecomp)
file(COPY "${DXIL_DLL}" DESTINATION "${CMAKE_BINARY_DIR}/XenosRecomp")
target_compile_definitions(XenosRecomp PRIVATE BLITZ_RECOMP)
