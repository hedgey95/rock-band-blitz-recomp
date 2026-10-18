// Native renderer phase N4: Xbox 360 textures from guest memory into host layouts.
#pragma once

#include <cstdint>
#include <string>
#include <vector>

#include <dxgiformat.h>

namespace blitz {

struct HostTexture {
  DXGI_FORMAT format = DXGI_FORMAT_UNKNOWN;
  uint32_t width = 0;   // level 0, in texels
  uint32_t height = 0;
  uint32_t array_size = 1;  // 6 for cube maps
  bool cube = false;
  uint32_t mip_levels = 1;
  // Fetch constant swizzle, 3 bits per destination component (xyzw): 0-3 pick
  // source component RGBA, 4 = 0, 5 = 1. The native renderer applies it with
  // the SRV's component mapping.
  uint32_t swizzle = 0;
  struct Subresource {  // D3D12 order: index = level + slice * mip_levels
    uint32_t width, height;  // in texels
    uint32_t row_pitch;      // bytes per row of blocks
    uint32_t rows;           // rows of blocks
    std::vector<uint8_t> data;
  };
  std::vector<Subresource> subresources;
};

// Decodes the texture a texture fetch constant describes: untiles every stored
// level (including the packed mip tail), swaps it to host byte order and
// converts formats the host lacks. `fetch` holds the six fetch constant dwords
// as the GPU sees them; `physical` points at guest physical address 0.
bool DecodeTexture(const uint32_t fetch[6], const uint8_t* physical, HostTexture* out,
                   std::string* error);

// Byte swap within each 16-bit word (1), 32-bit word (2) or 32-bit halves (3).
void SwapEndian(uint8_t* data, size_t size, uint32_t endian);

}  // namespace blitz
