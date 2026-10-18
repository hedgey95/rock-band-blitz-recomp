// Native renderer phase N5: vertex fetch descriptors; see vertex_input.h.

#include "vertex_input.h"

#include <algorithm>

namespace blitz {

namespace {

uint32_t WrittenMask(uint32_t sel) {
  uint32_t mask = 0;
  for (int c = 0; c < 4; ++c)
    if (((sel >> (3 * c)) & 7) != 7) mask |= 1u << c;
  return mask;
}

}  // namespace

VertexFetches BuildVertexFetches(const uint32_t* code, size_t dwords,
                                 const std::vector<VertexElement>& elements) {
  VertexFetches out;
  std::vector<VertexElement> sorted = elements;  // the translated shader numbers fetches by address
  std::sort(sorted.begin(), sorted.end(),
            [](const VertexElement& a, const VertexElement& b) { return a.address < b.address; });
  out.descriptors.assign(sorted.size(), {});
  // A mini fetch reads the stream of the last full fetch, with its stride.
  uint32_t fetch_constant = UINT32_MAX, stride = 0, rounded = 0;
  for (uint32_t slot : VertexFetchSlots(code, dwords)) {
    const uint32_t w0 = code[3 * slot], w1 = code[3 * slot + 1], w2 = code[3 * slot + 2];
    if (!((w1 >> 30) & 1)) {
      fetch_constant = ((w0 >> 20) & 0x1F) * 3 + ((w0 >> 25) & 3);
      stride = (w2 & 0xFF) * 4;
      rounded = (w1 >> 15) & 1;
    }
    if (fetch_constant == UINT32_MAX) continue;
    const uint32_t dst_reg = (w0 >> 12) & 0x3F, sel = w1 & 0xFFF;
    auto element = std::find_if(sorted.begin(), sorted.end(), [&](const VertexElement& v) {
      return v.dst_reg == dst_reg && WrittenMask(v.dst_sel) == WrittenMask(sel);
    });
    if (element == sorted.end()) continue;  // not a fetch the translated shader has
    auto stream = std::find(out.streams.begin(), out.streams.end(), fetch_constant);
    if (stream == out.streams.end()) {
      if (out.streams.size() == 4) {
        out.error = "more than 4 vertex streams";
        return out;
      }
      out.streams.push_back(fetch_constant);
      stream = out.streams.end() - 1;
    }
    auto& d = out.descriptors[element - sorted.begin()];
    d[0] = uint32_t(int32_t(w2 << 1) >> 9) * 4;  // signed 23-bit dword offset
    d[1] = stride | uint32_t(stream - out.streams.begin()) << 16 | rounded << 18 | 1u << 31;
    d[2] = ((w1 >> 16) & 0x3F)              // format
           | ((w1 >> 12) & 1) << 6          // signed
           | (((w1 >> 13) & 1) ^ 1) << 7    // normalized (not integer)
           | ((w1 >> 14) & 1) << 8          // signed "no zero" mode
           | ((w1 >> 24) & 0x3F) << 9       // exp adjust
           | sel << 16;                     // destination swizzle
  }
  out.valid = true;
  return out;
}

}  // namespace blitz
