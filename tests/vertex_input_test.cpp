// Self-check for the vertex fetch descriptors (src/renderer/vertex_input.h): a
// patched fetch that D3D moved and re-swizzled must describe the translated
// shader's fetch that writes the same register. Build target: blitz_tests.
#undef NDEBUG
#include <cassert>

#include "../src/renderer/vertex_input.h"

namespace {

uint32_t Sel(int x, int y, int z, int w) { return x | (y << 3) | (z << 6) | (w << 9); }

void TestSwizzledFetches() {
  // Control flow: exec of slots 1-2, both fetches. Then the two fetch slots.
  const uint64_t exec = 1 | (2u << 12) | (0b0101u << 16) | (1ull << 44);
  uint32_t code[9] = {uint32_t(exec), uint32_t(exec >> 32) & 0xFFFF, 0};
  // Slot 1: COLOR from 8888 at +0 into r1 as zyxw (D3DCOLOR), stream 95, 12-byte stride.
  code[3] = (1u << 12) | (31u << 20) | (2u << 25);
  code[4] = Sel(2, 1, 0, 3) | (6u << 16);
  code[5] = 3;
  // Slot 2: TEXCOORD from 16_16_FLOAT at +4 into r2 as yx (mini fetch: stream and stride of slot 1).
  code[6] = 2u << 12;
  code[7] = Sel(1, 0, 7, 7) | (31u << 16) | (1u << 30);
  code[8] = 1u << 8;
  // As compiled: TEXCOORD at slot 1 into r2.xy, COLOR at slot 2 into r1.xyzw.
  // The patch swapped their slots (by offset), so registers must decide.
  const std::vector<blitz::VertexElement> elements = {{2, 10, 0, 1, Sel(0, 1, 2, 3)},
                                                       {1, 5, 0, 2, Sel(0, 1, 7, 7)}};
  const auto fetches = blitz::BuildVertexFetches(code, 9, elements);
  assert(fetches.valid && fetches.streams.size() == 1 && fetches.streams[0] == 95);
  assert(fetches.descriptors.size() == 2);
  // Fetch 0 of the translated shader (address 1, TEXCOORD) is the patched mini fetch.
  const auto& uv = fetches.descriptors[0];
  assert(uv[0] == 4 && uv[1] == (12u | 1u << 31) && uv[2] == (31u | 1u << 7 | Sel(1, 0, 7, 7) << 16));
  // Fetch 1 (address 2, COLOR) is the patched full fetch.
  const auto& color = fetches.descriptors[1];
  assert(color[0] == 0 && color[1] == (12u | 1u << 31) && color[2] == (6u | 1u << 7 | Sel(2, 1, 0, 3) << 16));
}

}  // namespace

void RunVertexInputTests() { TestSwizzledFetches(); }
