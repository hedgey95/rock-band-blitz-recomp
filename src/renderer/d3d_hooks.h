// One hook per XDK D3D function, shared by everything that needs to see D3D
// calls (the native renderer, and the dev traces and captures). A guest
// function can only be hooked once, so features subscribe here instead of
// hooking. The list of hooked functions is in d3d_hooks.cpp.
#pragma once

#include <cstdint>

#include <rex/ppc/context.h>

namespace blitz::d3d {

// Guest addresses of the named D3D functions (config/functions.toml).
enum Function : uint32_t {
  kDrawVertices = 0x8251B220,
  kDrawIndexedVertices = 0x8251B638,
  kBeginVertices = 0x8251A7D8,
  kBeginIndexedVertices = 0x8251ACE0,
  kEndVertices = 0x8251A7C8,
  kDrawVerticesUP = 0x8251AC98,
  kFlushRegisters = 0x82521420,
  kFlushFloatConstants = 0x825217B8,
  kFlushFetchConstants = 0x82521660,
  kFlushClipPlanes = 0x825214E0,
  kFlushShaders = 0x82522550,
  kCreateShader = 0x82516108,
  kRegisterVertexShader = 0x82516010,
  kRegisterPixelShader = 0x82514420,
  kResolve = 0x8250C938,
  kClearF = 0x82519F50,
  kBeginTiling = 0x8251BD40,
  kSwap = 0x8251D580,  // the present: (dev, frontbuffer texture, ...)
  kKickOff = 0x8251F770,  // submits the command buffer segment, continues in the next
};

// Device fields (docs/d3d-device-map.md).
constexpr uint32_t kDeviceCommandPtr = 48;
constexpr uint32_t kDeviceFetchConstants = 1152;  // 32 slots x 6 dwords
constexpr uint32_t kDeviceBoundPixelShader = 13068;
constexpr uint32_t kDeviceBoundVertexShader = 13072;
constexpr uint32_t kDeviceIndexBuffer = 12812;
constexpr uint32_t kDeviceRegisters2000 = 10368;  // shadow of registers 0x2000-0x200F

// Sees every hooked call: Before with the arguments, After with the result
// (registers other than r1, r2 and r13-r31 are clobbered by then, so a
// listener that needs arguments in After keeps them from Before).
class Listener {
 public:
  virtual ~Listener() = default;
  virtual void Before(uint32_t function, PPCContext& ctx, uint8_t* base) {}
  virtual void After(uint32_t function, PPCContext& ctx, uint8_t* base) {}
};

// Call during static initialization (before the game runs); never removed.
void AddListener(Listener* listener);

}  // namespace blitz::d3d
