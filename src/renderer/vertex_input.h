// Native renderer phase N5: vertex fetch descriptors.
//
// The translated vertex shaders fetch their vertices themselves, from the
// guest streams as raw bytes (BlitzVertexFetch, patches/xenos-recomp.diff), as
// the GPU does: Blitz's shaders compute fetch indices (instancing from the
// vertex index, indices read from another stream), which an input assembler
// cannot express. D3D patches each fetch when it binds the shader to a vertex
// declaration (format, stream, offset, stride, swizzle), and it reorders the
// fetches by offset, moving their destination registers with them (Xenia runs
// the same patched code). So each patched fetch is matched to the translated
// shader's fetch by destination register and written components, and its
// parameters fill that fetch's descriptor.
#pragma once

#include <array>
#include <cstdint>
#include <string>
#include <vector>

#include "shader_container.h"

namespace blitz {

struct VertexFetches {
  std::vector<uint32_t> streams;  // vertex fetch constant (0..95) per stream, at most 4
  // Per fetch of the translated shader, in address order: see BlitzVertexFetch.
  // The stream's endian (y bits 20-21) and size (w) are filled per draw.
  std::vector<std::array<uint32_t, 4>> descriptors;
  bool valid = false;
  std::string error;
};

// `code`: the patched microcode in host order; `elements`: the container's.
VertexFetches BuildVertexFetches(const uint32_t* code, size_t dwords,
                                 const std::vector<VertexElement>& elements);

}  // namespace blitz
