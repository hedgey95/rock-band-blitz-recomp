// Native renderer phase N3: capture the game's shaders as XDK shader containers
// for XenosRecomp to translate offline.
//
//   --capture_shaders=<dir>   (outside the repo: these are game data)
//
// Harmonix's loader (sub_823EC540) reads ready-made D3D shader objects, which
// embed the container's virtual part (header, definitions, shader descriptor),
// and a separate microcode blob, then registers each object with its microcode
// address: D3D_RegisterVertexShader (container at object+872) and
// D3D_RegisterPixelShader (object+40). D3D_CreateShader takes a whole container.
//
// The game registers its whole shader library at load (about 4,000 shaders), so
// a registered object is captured the first time D3D_FlushShaders binds it for a
// draw, before the flush runs. That is also before D3D patches the vertex
// fetches in place for the vertex declaration (stream layout, and swizzles for
// 16-bit texcoords that XenosRecomp handles itself). The container is written
// once as [virtual part][physical part] (the layout XenosRecomp reads), named
// <fnv1a-64 of the bytes>.<vs|ps>.xsc, and the name is appended to
// <dir>/bound.txt. A bound object that was never registered is logged as a
// warning.

#include <cstdio>
#include <filesystem>
#include <mutex>
#include <string>
#include <unordered_map>
#include <unordered_set>

#include <rex/cvar.h>
#include <rex/logging.h>
#include <rex/memory/utils.h>
#include <rex/system/kernel_state.h>
#include <rex/system/xmemory.h>

#include "d3d_hooks.h"

REXCVAR_DEFINE_STRING(capture_shaders, "", "Blitz",
                      "Dev: write each shader the game draws with as an XDK container into this directory");

namespace {

namespace d3d = blitz::d3d;

struct Parts {
  uint32_t virtual_part;   // container header
  uint32_t physical_part;  // microcode address
};

std::mutex g_mutex;
std::unordered_map<uint32_t, Parts> g_pending;  // registered, not bound yet
std::unordered_set<uint32_t> g_captured;         // objects captured since registration
std::unordered_set<std::string> g_written;
std::unordered_set<uint32_t> g_unknown;

const std::string& Dir() {
  static const std::string dir = REXCVAR_GET(capture_shaders);
  return dir;
}

uint32_t Load(uint8_t* base, uint32_t address) {
  return rex::memory::load_and_swap<uint32_t>(base + address);
}

// The microcode as the GPU reads it: D3D turns the object's address into a
// physical one (0xE0000000+ addresses are offset by 4 KB) for IM_LOAD. Reading
// the virtual address directly gives other bytes for these addresses.
const char* Microcode(uint32_t address) {
  const uint32_t physical = (address & 0x1FFFFFFF) + ((((address >> 20) & 0xFFF) + 0x200) & 0x1000);
  return REX_KERNEL_STATE()->memory()->TranslatePhysical<const char*>(physical);
}

void Register(uint32_t object, Parts parts) {
  std::lock_guard lock(g_mutex);
  g_pending[object] = parts;
  g_captured.erase(object);
}

void Append(const char* file, const std::string& line) {
  if (FILE* f = std::fopen((std::filesystem::path(Dir()) / file).string().c_str(), "a")) {
    std::fprintf(f, "%s\n", line.c_str());
    std::fclose(f);
  }
}

void CaptureBound(uint8_t* base, uint32_t device, uint32_t field) {
  const uint32_t object = Load(base, device + field);
  if (!object || g_captured.count(object)) return;
  auto it = g_pending.find(object);
  if (it == g_pending.end()) {
    if (g_unknown.insert(object).second)
      REXLOG_WARN("capture_shaders: bound shader object {:08X} was never registered", object);
    return;
  }
  const Parts parts = it->second;
  g_pending.erase(it);
  g_captured.insert(object);

  const uint32_t flags = Load(base, parts.virtual_part);
  if ((flags & 0xFFFFFF00) != 0x102A1100) {
    REXLOG_WARN("capture_shaders: no container at {:08X}", parts.virtual_part);
    return;
  }
  std::string bytes(reinterpret_cast<const char*>(base + parts.virtual_part),
                    Load(base, parts.virtual_part + 4));
  bytes.append(Microcode(parts.physical_part), Load(base, parts.virtual_part + 8));
  uint64_t hash = 0xCBF29CE484222325ull;
  for (char c : bytes) hash = (hash ^ uint8_t(c)) * 0x100000001B3ull;
  char name[64];
  std::snprintf(name, sizeof(name), "%016llX.%s", static_cast<unsigned long long>(hash),
                (flags & 1) ? "vs" : "ps");
  if (!g_written.insert(name).second) return;
  Append("bound.txt", name);
  const auto path = std::filesystem::path(Dir()) / (std::string(name) + ".xsc");
  if (FILE* f = std::fopen(path.string().c_str(), "wb")) {
    std::fwrite(bytes.data(), 1, bytes.size(), f);
    std::fclose(f);
  } else {
    REXLOG_ERROR("capture_shaders: cannot write {}", path.string());
  }
}

class ShaderCapture final : public d3d::Listener {
 public:
  ShaderCapture() { d3d::AddListener(this); }

  void Before(uint32_t function, PPCContext& ctx, uint8_t* base) override {
    if (Dir().empty()) return;
    switch (function) {
      case d3d::kRegisterVertexShader:
        Register(ctx.r3.u32, {ctx.r3.u32 + 872, ctx.r4.u32});
        break;
      case d3d::kRegisterPixelShader:
        Register(ctx.r3.u32, {ctx.r3.u32 + 40, ctx.r4.u32});
        break;
      case d3d::kCreateShader:
        t_container = ctx.r3.u32;
        break;
      case d3d::kFlushShaders: {
        std::lock_guard lock(g_mutex);
        CaptureBound(base, ctx.r3.u32, d3d::kDeviceBoundVertexShader);
        CaptureBound(base, ctx.r3.u32, d3d::kDeviceBoundPixelShader);
        break;
      }
      default:
        break;
    }
  }

  void After(uint32_t function, PPCContext& ctx, uint8_t* base) override {
    // Vertex shaders made by D3D_CreateShader also pass through
    // D3D_RegisterVertexShader. Pixel shaders keep copies in the object:
    // container at +40, microcode address at +24.
    const uint32_t object = ctx.r3.u32;
    if (function == d3d::kCreateShader && !Dir().empty() && object &&
        !(Load(base, t_container) & 1))
      Register(object, {object + 40, Load(base, object + 24)});
  }

 private:
  static thread_local uint32_t t_container;
};

thread_local uint32_t ShaderCapture::t_container = 0;

ShaderCapture g_shader_capture;

}  // namespace
