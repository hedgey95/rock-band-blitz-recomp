// Native renderer phase N4: Xbox 360 textures from guest memory into host layouts.
//
// The guest layout (pitches, mip offsets, the packed mip tail, tiled
// addressing) comes from the SDK's Xenia-derived texture_util, compiled into
// this executable (BSD-licensed format math, not GPU emulation). The formats
// handled are the ones Blitz uses, measured with tools/resource_inventory.py.

#include "texture_decode.h"

#include <algorithm>
#include <cstring>
#include <utility>

#include <rex/graphics/pipeline/texture/info.h>
#include <rex/graphics/pipeline/texture/util.h>
#include <rex/graphics/xenos.h>

namespace blitz {

namespace xenos = rex::graphics::xenos;
namespace texture_util = rex::graphics::texture_util;
using rex::graphics::FormatInfo;

namespace {

struct HostFormat {
  DXGI_FORMAT format;
  bool unorm32_to_float;  // k_32_32_32_32: 32-bit UNORM, which no host format has
};

bool HostFormatFor(xenos::TextureFormat format, HostFormat* out) {
  switch (format) {
    case xenos::TextureFormat::k_DXT1:
      *out = {DXGI_FORMAT_BC1_UNORM, false};
      return true;
    case xenos::TextureFormat::k_DXT2_3:
      *out = {DXGI_FORMAT_BC2_UNORM, false};
      return true;
    case xenos::TextureFormat::k_DXT4_5:
      *out = {DXGI_FORMAT_BC3_UNORM, false};
      return true;
    case xenos::TextureFormat::k_DXN:
      *out = {DXGI_FORMAT_BC5_UNORM, false};
      return true;
    case xenos::TextureFormat::k_8_8_8_8:
      *out = {DXGI_FORMAT_R8G8B8A8_UNORM, false};
      return true;
    case xenos::TextureFormat::k_32_32_32_32:
      *out = {DXGI_FORMAT_R32G32B32A32_FLOAT, true};
      return true;
    default:
      return false;
  }
}

}  // namespace

void SwapEndian(uint8_t* data, size_t size, uint32_t endian) {
  switch (endian) {
    case 1:  // 8in16
      for (size_t i = 0; i + 1 < size; i += 2) std::swap(data[i], data[i + 1]);
      break;
    case 2:  // 8in32
      for (size_t i = 0; i + 3 < size; i += 4) {
        std::swap(data[i], data[i + 3]);
        std::swap(data[i + 1], data[i + 2]);
      }
      break;
    case 3:  // 16in32
      for (size_t i = 0; i + 3 < size; i += 4) {
        std::swap(data[i], data[i + 2]);
        std::swap(data[i + 1], data[i + 3]);
      }
      break;
    default:
      break;
  }
}

bool DecodeTexture(const uint32_t fetch_dwords[6], const uint8_t* physical, HostTexture* out,
                   std::string* error) {
  xenos::xe_gpu_texture_fetch_t fetch;
  std::memcpy(&fetch, fetch_dwords, sizeof(fetch));
  if (fetch.type != xenos::FetchConstantType::kTexture) {
    *error = "not a texture fetch constant";
    return false;
  }
  HostFormat host;
  if (!HostFormatFor(fetch.format, &host)) {
    *error = "unsupported format " + std::to_string(uint32_t(fetch.format));
    return false;
  }
  const bool cube = fetch.dimension == xenos::DataDimension::kCube;
  if (fetch.dimension != xenos::DataDimension::k2DOrStacked && !cube) {
    *error = "unsupported dimension " + std::to_string(uint32_t(fetch.dimension));
    return false;
  }

  uint32_t width_minus_1, height_minus_1, depth_minus_1, base_page, mip_page, mip_min, mip_max;
  texture_util::GetSubresourcesFromFetchConstant(fetch, &width_minus_1, &height_minus_1,
                                                 &depth_minus_1, &base_page, &mip_page, &mip_min,
                                                 &mip_max);
  if (!base_page) {
    *error = "no base level";
    return false;
  }
  const uint32_t width = width_minus_1 + 1, height = height_minus_1 + 1;
  const uint32_t array_size = depth_minus_1 + 1;
  if (!mip_page) mip_max = 0;
  const auto layout = texture_util::GetGuestTextureLayout(
      fetch.dimension, fetch.pitch, width, height, array_size, fetch.tiled, fetch.format,
      fetch.packed_mips, true, mip_max);

  const FormatInfo* info = FormatInfo::Get(fetch.format);
  const uint32_t block_w = info->block_width, block_h = info->block_height;
  const uint32_t bpb = info->bytes_per_block();
  uint32_t bpb_log2 = 0;
  while ((1u << bpb_log2) < bpb) ++bpb_log2;

  out->format = host.format;
  out->width = width;
  out->height = height;
  out->array_size = array_size;
  out->cube = cube;
  out->mip_levels = layout.max_level + 1;
  out->swizzle = fetch.swizzle;
  out->subresources.assign(out->mip_levels * array_size, {});

  for (uint32_t slice = 0; slice < array_size; ++slice) {
    for (uint32_t level = 0; level <= layout.max_level; ++level) {
      // The level's storage: the base, a mip, or the packed tail it lives in.
      const bool from_mips = level != 0;
      const uint32_t stored = std::min(level, layout.packed_level);
      const auto& storage = from_mips ? layout.mips[stored] : layout.base;
      uint32_t address = (from_mips ? (mip_page << 12) + layout.mip_offsets_bytes[stored]
                                    : base_page << 12) +
                         slice * storage.array_slice_stride_bytes;
      uint32_t offset_x = 0, offset_y = 0, offset_z = 0;
      if (level >= layout.packed_level)
        texture_util::GetPackedMipOffset(width, height, 1, fetch.format, level, offset_x, offset_y,
                                         offset_z);

      const uint32_t level_w = std::max(width >> level, 1u), level_h = std::max(height >> level, 1u);
      const uint32_t blocks_x = (level_w + block_w - 1) / block_w;
      const uint32_t blocks_y = (level_h + block_h - 1) / block_h;
      auto& sub = out->subresources[level + slice * out->mip_levels];
      sub.width = level_w;
      sub.height = level_h;
      sub.rows = blocks_y;
      sub.row_pitch = blocks_x * (host.unorm32_to_float ? 16 : bpb);
      sub.data.resize(size_t(sub.row_pitch) * blocks_y);

      const uint32_t pitch_blocks = storage.row_pitch_bytes / bpb;
      for (uint32_t y = 0; y < blocks_y; ++y) {
        for (uint32_t x = 0; x < blocks_x; ++x) {
          const uint32_t gx = x + offset_x, gy = y + offset_y;
          const uint32_t guest =
              address + (fetch.tiled ? uint32_t(texture_util::GetTiledOffset2D(
                                           int32_t(gx), int32_t(gy), pitch_blocks, bpb_log2))
                                     : gy * storage.row_pitch_bytes + gx * bpb);
          uint8_t* dst = sub.data.data() + size_t(y) * sub.row_pitch + size_t(x) * bpb;
          std::memcpy(dst, physical + guest, bpb);
          SwapEndian(dst, bpb, uint32_t(fetch.endianness));
          if (host.unorm32_to_float) {
            for (int c = 0; c < 4; ++c) {
              uint32_t v;
              std::memcpy(&v, dst + 4 * c, 4);
              float f = float(double(v) / 4294967295.0);
              std::memcpy(dst + 4 * c, &f, 4);
            }
          }
        }
      }
    }
  }
  return true;
}

}  // namespace blitz
