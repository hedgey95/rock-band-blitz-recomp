// Native renderer phase N4: D3D operations other than draws, decoded from the
// arguments of the game's calls (argument order read from the asm; this XDK
// build differs from the documented order).
#pragma once

#include <cstdint>

#include <rex/ppc/context.h>

namespace blitz::d3d {

// D3DDevice_Resolve: copies a render target (or depth) region to a texture,
// optionally clearing the render target and/or depth afterwards.
struct ResolveOp {
  uint32_t flags;
  uint32_t source;  // 0-3: render target, 4: depth/stencil
  bool clear_color, clear_depth;
  bool has_rect;
  int32_t rect[4];  // x1, y1, x2, y2 (without a rect: the destination's size)
  uint32_t dest_texture;   // guest address of the texture object
  uint32_t dest_fetch[6];  // its fetch constant (object+28) as the GPU sees it, host order
  int32_t dest_point[2];
  uint32_t dest_level, dest_slice;
  float clear_color_value[4];
  float clear_z;
  uint32_t clear_stencil;
  // RB_COPY_DEST_INFO copy_dest_swap: red and blue swapped on the way out (set
  // by the renderer from the packets the call wrote, not by DecodeResolve).
  bool swap_rb = false;
};

// D3DDevice_ClearF.
struct ClearOp {
  uint32_t flags;
  bool has_rect;
  int32_t rect[4];
  float color[4];
  float z;
  uint32_t stencil;
};

// Decode from the context at entry (the hook's Before).
ResolveOp DecodeResolve(const PPCContext& ctx, const uint8_t* base);
ClearOp DecodeClear(const PPCContext& ctx, const uint8_t* base);

}  // namespace blitz::d3d
