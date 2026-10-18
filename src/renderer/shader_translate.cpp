// Shaders missing from --native_shader_dir, translated inside the game: no
// helper programs are launched. XenosRecomp's translator
// (thirdparty/XenosRecomp/XenosRecomp/shader_recompiler.cpp, with
// patches/xenos-recomp.diff) is built into blitz.exe with shader_common.h
// embedded, and dxc runs through dxcompiler.dll (copied next to the exe with
// dxil.dll, which signs the DXIL) with the arguments tools/translate_shaders.py
// gives dxc.exe, so the output matches the offline set.
#include "shader_translate.h"

#include <windows.h>
#include <objbase.h>  // IUnknown and IStream for dxcapi.h (WIN32_LEAN_AND_MEAN leaves them out)

#include "pch.h"  // XenosRecomp's: dxcapi.h, fmt, be<T>
#include "shader_recompiler.h"

#include <algorithm>
#include <chrono>
#include <fstream>
#include <iterator>
#include <semaphore>
#include <thread>

#include <rex/logging.h>

#include "shader_common_bytes.h"  // generated: kShaderCommon

namespace blitz {

namespace {

void WriteFile(const std::filesystem::path& path, const void* data, size_t size) {
  std::ofstream(path, std::ios::binary).write(static_cast<const char*>(data), std::streamsize(size));
}

// dxc for one target; DXIL, or empty with dxc's messages in `errors`.
std::vector<uint8_t> Compile(const std::string& hlsl, const wchar_t* target, std::string& errors) {
  thread_local IDxcCompiler3* compiler = [] {
    IDxcCompiler3* created = nullptr;
    DxcCreateInstance(CLSID_DxcCompiler, IID_PPV_ARGS(&created));
    return created;
  }();
  if (!compiler) {
    errors = "dxcompiler.dll could not be loaded";
    return {};
  }
  const wchar_t* args[] = {L"-T",     target, L"-HV", L"2021", L"-all-resources-bound", L"-Wno-ignored-attributes",
                           L"-Qstrip_reflect", L"-Qstrip_debug"};
  const DxcBuffer source{hlsl.data(), hlsl.size(), DXC_CP_ACP};
  std::vector<uint8_t> dxil;
  IDxcResult* result = nullptr;
  if (FAILED(compiler->Compile(&source, args, UINT32(std::size(args)), nullptr, IID_PPV_ARGS(&result))) || !result)
    return dxil;
  HRESULT status = E_FAIL;
  result->GetStatus(&status);
  IDxcBlob* object = nullptr;
  if (SUCCEEDED(status) && SUCCEEDED(result->GetOutput(DXC_OUT_OBJECT, IID_PPV_ARGS(&object), nullptr)) && object) {
    const auto* bytes = static_cast<const uint8_t*>(object->GetBufferPointer());
    dxil.assign(bytes, bytes + object->GetBufferSize());
    object->Release();
  }
  IDxcBlobUtf8* messages = nullptr;
  if (dxil.empty() && SUCCEEDED(result->GetOutput(DXC_OUT_ERRORS, IID_PPV_ARGS(&messages), nullptr)) && messages) {
    errors.assign(messages->GetStringPointer(), messages->GetStringLength());
    messages->Release();
  }
  result->Release();
  return dxil;
}

}  // namespace

std::vector<uint8_t> TranslateShader(const std::filesystem::path& dir, const std::string& name,
                                     const std::string& container) {
  // A few at a time, below the game's priority, so a burst of new shaders leaves the game its cores.
  static std::counting_semaphore<> slots(std::max(1u, std::thread::hardware_concurrency() / 4));
  slots.acquire();
  SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_BELOW_NORMAL);
  const auto start = std::chrono::steady_clock::now();
  const auto file = [&](const char* extension) { return dir / (name + extension); };
  WriteFile(file(".xsc"), container.data(), container.size());

  std::string hlsl;
  try {
    ShaderRecompiler recompiler;
    recompiler.recompile(reinterpret_cast<const uint8_t*>(container.data()),
                         std::string_view(reinterpret_cast<const char*>(kShaderCommon), sizeof(kShaderCommon)));
    hlsl = std::move(recompiler.out);
  } catch (...) {
    hlsl.clear();
  }
  std::vector<uint8_t> dxil;
  std::string errors, target;
  if (!hlsl.empty()) {
    WriteFile(file(".hlsl"), hlsl.data(), hlsl.size());
    for (const wchar_t* t : {name.ends_with(".vs") ? L"vs_6_0" : L"ps_6_0", L"lib_6_3"}) {
      target = std::filesystem::path(t).string();
      dxil = Compile(hlsl, t, errors);
      if (!dxil.empty()) break;
    }
  }
  std::error_code error;
  if (!dxil.empty()) {
    // Written under a temporary name so an interrupted write never leaves a bad cache entry.
    WriteFile(file(".dxil.tmp"), dxil.data(), dxil.size());
    std::filesystem::rename(file(".dxil.tmp"), file(".dxil"), error);
    std::filesystem::remove(file(".dxc.log"), error);
  } else if (!errors.empty()) {
    WriteFile(file(".dxc.log"), errors.data(), errors.size());
  }
  slots.release();

  const double seconds = std::chrono::duration<double>(std::chrono::steady_clock::now() - start).count();
  if (dxil.empty())
    REXLOG_WARN("native_renderer: could not translate shader {} ({:.1f} s): {}", name, seconds,
                hlsl.empty() ? std::string("XenosRecomp failed") : "dxc failed, see " + file(".dxc.log").string());
  else
    REXLOG_INFO("native_renderer: translated shader {} ({}, {:.1f} s)", name, target, seconds);
  return dxil;
}

}  // namespace blitz
