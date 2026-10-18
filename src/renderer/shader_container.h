// XDK shader containers as the game holds them: reading, naming, and the parts
// of their layout the native renderer needs. Shared by the shader capture and
// the renderer so both name a shader the same way.
#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace blitz {

// Virtual address -> the physical one D3D gives the GPU (0xE0000000+ addresses
// are offset by 4 KB).
inline uint32_t PhysicalAddress(uint32_t address) {
  return (address & 0x1FFFFFFF) + ((((address >> 20) & 0xFFF) + 0x200) & 0x1000);
}

// [virtual part][physical part] (the layout XenosRecomp reads), from the
// object's container header and its microcode address; empty if the header is
// not a container. `physical` points at guest physical address 0.
std::string ReadContainer(const uint8_t* base, const uint8_t* physical, uint32_t virtual_part,
                          uint32_t microcode_address);

// <fnv1a-64 of the bytes>.<vs|ps>, the name the capture and the translated
// shader files use.
std::string ContainerName(const std::string& container);

struct VertexElement {
  uint32_t address;  // vertex fetch instruction slot in the container's microcode
  uint32_t usage;    // D3DDECLUSAGE
  uint32_t usage_index;
  // That fetch as compiled: destination register and swizzle (3 bits per
  // component: 0-3 source component, 4 zero, 5 one, 7 not written). D3D moves
  // and re-swizzles fetches when it patches them, so these identify the input.
  uint32_t dst_reg, dst_sel;
};

// The vertex shader's inputs (empty for pixel shaders), from the container as
// compiled (before D3D patches the microcode at bind).
std::vector<VertexElement> VertexElements(const std::string& container);

// Instruction slots (3 dwords each) of the vertex fetches in host-order
// microcode, found through the control flow's exec instructions.
std::vector<uint32_t> VertexFetchSlots(const uint32_t* code, size_t dwords);

}  // namespace blitz
