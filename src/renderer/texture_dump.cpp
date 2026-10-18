// Dev aid for native-renderer phase N4: writes every texture the game binds,
// decoded by texture_decode.cpp, as a DDS file (all mips and cube faces).
//
//   --dump_textures=<dir>   (outside the repo: these are game data)
//
// Textures are taken when D3D_FlushFetchConstants sends a texture fetch
// constant to the GPU, from the device's fetch constant shadow. Each distinct
// texture (address, format, size, tiling, mips) is written once, as
// <base address>_<format>_<w>x<h>[_cube]_sw<swizzle>.dds; the fetch constant
// swizzle is in the name because DDS has no field for it.
// tools/texture_sheet.py turns a dump directory into a contact sheet.

#include <cstdio>
#include <filesystem>
#include <mutex>
#include <set>
#include <string>
#include <tuple>

#include <rex/cvar.h>
#include <rex/logging.h>
#include <rex/memory/utils.h>
#include <rex/system/kernel_state.h>
#include <rex/system/xmemory.h>

#include "d3d_hooks.h"
#include "texture_decode.h"

REXCVAR_DEFINE_STRING(dump_textures, "", "Blitz", "Dev: write each texture the game binds as DDS here");

namespace {

namespace d3d = blitz::d3d;

void WriteDds(const std::filesystem::path& path, const blitz::HostTexture& t) {
  FILE* f = std::fopen(path.string().c_str(), "wb");
  if (!f) {
    REXLOG_ERROR("dump_textures: cannot write {}", path.string());
    return;
  }
  uint32_t header[32] = {};
  header[0] = 0x20534444;  // "DDS "
  header[1] = 124;
  header[2] = 0x1 | 0x2 | 0x4 | 0x1000 | 0x20000;  // caps, height, width, pixel format, mip count
  header[3] = t.height;
  header[4] = t.width;
  header[7] = t.mip_levels;
  header[19] = 32;          // pixel format size
  header[20] = 0x4;         // FOURCC
  header[21] = 0x30315844;  // "DX10"
  header[27] = 0x1000 | 0x400000 | 0x8;  // texture, mipmap, complex
  if (t.cube) header[28] = 0x200 | 0xFC00;  // cubemap, all faces
  const uint32_t dx10[5] = {uint32_t(t.format), 3 /* TEXTURE2D */, t.cube ? 0x4u : 0u,
                            t.cube ? t.array_size / 6 : t.array_size, 0};
  std::fwrite(header, sizeof(header), 1, f);
  std::fwrite(dx10, sizeof(dx10), 1, f);
  for (const auto& sub : t.subresources) std::fwrite(sub.data.data(), 1, sub.data.size(), f);
  std::fclose(f);
}

class TextureDump final : public d3d::Listener {
 public:
  TextureDump() { d3d::AddListener(this); }

  void Before(uint32_t function, PPCContext& ctx, uint8_t* base) override {
    if (function == d3d::kFlushFetchConstants) {
      t_device = ctx.r3.u32;
      t_mask = ctx.r4.u64;
    }
  }

  void After(uint32_t function, PPCContext& ctx, uint8_t* base) override {
    if (function != d3d::kFlushFetchConstants) return;
    static const std::string dir = REXCVAR_GET(dump_textures);
    if (dir.empty()) return;
    for (uint32_t slot = 0; slot < 32; ++slot) {
      if (!((t_mask >> (63 - slot)) & 1)) continue;  // bit 63 is slot 0
      uint32_t fetch[6];
      for (int i = 0; i < 6; ++i)
        fetch[i] = rex::memory::load_and_swap<uint32_t>(
            base + t_device + d3d::kDeviceFetchConstants + 24 * slot + 4 * i);
      if ((fetch[0] & 3) != 2) continue;  // not a texture
      Dump(dir, fetch);
    }
  }

 private:
  void Dump(const std::string& dir, const uint32_t (&fetch)[6]) {
    // Identity: tiling and pitch, format and address, size, mip address.
    const auto key = std::make_tuple(fetch[0] & 0xFFC00000u, fetch[1], fetch[2], fetch[5] & 0xFFFFF800u);
    {
      std::lock_guard lock(mutex_);
      if (!seen_.insert(key).second) return;
    }
    blitz::HostTexture texture;
    std::string error;
    const auto* physical = REX_KERNEL_STATE()->memory()->TranslatePhysical<const uint8_t*>(0);
    if (!blitz::DecodeTexture(fetch, physical, &texture, &error)) {
      REXLOG_WARN("dump_textures: {:08X} format {}: {}", fetch[1] & 0xFFFFF000, fetch[1] & 0x3F,
                  error);
      return;
    }
    char name[128];
    std::snprintf(name, sizeof(name), "%08X_f%u_%ux%u%s_sw%03X.dds", fetch[1] & 0xFFFFF000,
                  fetch[1] & 0x3F, texture.width, texture.height, texture.cube ? "_cube" : "",
                  texture.swizzle);
    WriteDds(std::filesystem::path(dir) / name, texture);
  }

  std::mutex mutex_;
  std::set<std::tuple<uint32_t, uint32_t, uint32_t, uint32_t>> seen_;
  static thread_local uint32_t t_device;
  static thread_local uint64_t t_mask;
};

thread_local uint32_t TextureDump::t_device = 0;
thread_local uint64_t TextureDump::t_mask = 0;

TextureDump g_texture_dump;

}  // namespace
