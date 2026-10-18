// XDK shader containers; see shader_container.h. Layout as XenosRecomp reads it
// (thirdparty/XenosRecomp/XenosRecomp/shader.h): big-endian header with
// virtualSize at +4, physicalSize at +8, shaderOffset at +0x18; the vertex
// shader descriptor keeps its vertex elements from index field18 of the
// trailing array.

#include "shader_container.h"

#include <algorithm>
#include <cstdio>
#include <cstring>

#include <rex/memory/utils.h>

namespace blitz {

namespace {

uint32_t Big(const std::string& bytes, size_t offset) {
  if (offset + 4 > bytes.size()) return 0;
  return rex::memory::load_and_swap<uint32_t>(bytes.data() + offset);
}

}  // namespace

std::string ReadContainer(const uint8_t* base, const uint8_t* physical, uint32_t virtual_part,
                          uint32_t microcode_address) {
  const uint32_t flags = rex::memory::load_and_swap<uint32_t>(base + virtual_part);
  if ((flags & 0xFFFFFF00) != 0x102A1100) return {};
  const uint32_t virtual_size = rex::memory::load_and_swap<uint32_t>(base + virtual_part + 4);
  const uint32_t physical_size = rex::memory::load_and_swap<uint32_t>(base + virtual_part + 8);
  std::string bytes(reinterpret_cast<const char*>(base + virtual_part), virtual_size);
  // Read the microcode where the GPU does; the plain virtual address gives other bytes.
  bytes.append(reinterpret_cast<const char*>(physical + PhysicalAddress(microcode_address)),
               physical_size);
  return bytes;
}

std::string ContainerName(const std::string& container) {
  uint64_t hash = 0xCBF29CE484222325ull;
  for (char c : container) hash = (hash ^ uint8_t(c)) * 0x100000001B3ull;
  char name[64];
  std::snprintf(name, sizeof(name), "%016llX.%s", static_cast<unsigned long long>(hash),
                (Big(container, 0) & 1) ? "vs" : "ps");
  return name;
}

std::vector<VertexElement> VertexElements(const std::string& container) {
  std::vector<VertexElement> elements;
  if (!(Big(container, 0) & 1)) return elements;
  const uint32_t shader = Big(container, 0x18);
  const uint32_t first = Big(container, shader + 0x18);  // field18
  const uint32_t count = Big(container, shader + 0x1C);  // vertexElementCount
  // The microcode: (offset, size) at the shader descriptor, within the physical part.
  const uint32_t code = Big(container, 4) + Big(container, shader);
  for (uint32_t i = 0; i < count; ++i) {
    const uint32_t value = Big(container, shader + 0x24 + 4 * (first + i));
    const uint32_t address = value & 0xFFF;
    const uint32_t w0 = Big(container, code + 12 * address), w1 = Big(container, code + 12 * address + 4);
    elements.push_back({address, (value >> 12) & 0xF, (value >> 16) & 0xF, (w0 >> 12) & 0x3F, w1 & 0xFFF});
  }
  return elements;
}

std::vector<uint32_t> VertexFetchSlots(const uint32_t* code, size_t dwords) {
  // Exec opcodes: exec, exec end, cond exec (end), cond exec pred (end),
  // cond exec pred clean (end).
  auto is_exec = [](uint64_t cf) {
    const uint32_t op = uint32_t(cf >> 44) & 0xF;
    return op == 1 || op == 2 || op == 3 || op == 4 || op == 5 || op == 6 || op == 13 || op == 14;
  };
  std::vector<uint32_t> slots;
  size_t first_instruction = dwords / 3;
  for (size_t pair = 0; pair < first_instruction && 3 * pair + 2 < dwords; ++pair) {
    const uint64_t d0 = code[3 * pair], d1 = code[3 * pair + 1], d2 = code[3 * pair + 2];
    for (uint64_t cf : {d0 | ((d1 & 0xFFFF) << 32), (d1 >> 16) | (d2 << 16)}) {
      if (!is_exec(cf)) continue;
      const uint32_t address = uint32_t(cf & 0xFFF), count = uint32_t(cf >> 12) & 7;
      const uint32_t sequence = uint32_t(cf >> 16) & 0xFFF;
      if (address) first_instruction = std::min<size_t>(first_instruction, address);
      for (uint32_t i = 0; i < count; ++i) {
        const uint32_t slot = address + i;
        if (((sequence >> (2 * i)) & 1) && 3 * size_t(slot) < dwords && (code[3 * slot] & 0x1F) == 0)
          slots.push_back(slot);
      }
    }
  }
  return slots;
}

}  // namespace blitz
