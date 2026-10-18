// The display gamma ramp the game writes through the DC_LUT registers (MMIO),
// kept as the SDK's command processor keeps it, for the present pass to apply:
// a 256-entry table of 10-bit values, or a 128-segment piecewise-linear ramp
// (DC_LUT_RW_MODE bit 0 picks which one is used).
#pragma once

#include <cstdint>
#include <cstring>
#include <mutex>

namespace blitz {

class GammaRamp {
 public:
  // The present pass's constants: mode (0 table, 1 PWL), then 256 uint4. Table:
  // x = DC_LUT_30_COLOR (blue 0:9, green 10:19, red 20:29). PWL: entries 0-127,
  // xyz = red, green, blue as base | delta << 16.
  struct Constants {
    uint32_t mode, pad[3];
    uint32_t entries[256][4];
  };

  GammaRamp() {
    // Linear, as the SDK starts (what games set for sRGB).
    for (uint32_t i = 0; i < 256; ++i) {
      const uint32_t v = i * 0x3FF / 0xFF;
      table_[i] = v | v << 10 | v << 20;
    }
    for (uint32_t i = 0; i < 128; ++i)
      for (uint32_t c = 0; c < 3; ++c)
        pwl_[i][c] = ((i * 0xFFFF / 0x7F) & ~0x3Fu) | (i < 0x7F ? 0x200u : 0u) << 16;
  }

  // An MMIO register write (index in dwords), after the register file has `value`.
  void Write(uint32_t index, uint32_t value, const uint32_t* regs) {
    std::lock_guard lock(mutex_);
    const uint32_t rw_index = regs[kRwIndex] & 0xFF;
    // DC_LUT_SEQ_COLOR and DC_LUT_PWL_DATA are R, G, B; the write enable mask is B, G, R.
    const bool enabled = (regs[kWriteEnMask] >> (2 - component_)) & 1;
    switch (index) {
      case kRwIndex:
        component_ = 0;
        break;
      case kSeqColor: {
        const uint32_t v = (value & 0xFFFF) >> 6, shift = 20 - 10 * component_;
        if (enabled) table_[rw_index] = (table_[rw_index] & ~(0x3FFu << shift)) | v << shift;
        Advance(regs, rw_index);
        break;
      }
      case kPwlData:
        if (enabled) pwl_[rw_index & 0x7F][component_] = value & 0xFFC0FFC0u;
        Advance(regs, rw_index);
        break;
      case kColor30:
        table_[rw_index] = value & 0x3FFFFFFF;
        const_cast<uint32_t*>(regs)[kRwIndex] = (rw_index + 1) & 0xFF;
        break;
      default:
        break;
    }
  }

  static bool Watches(uint32_t index) { return index >= kRwMode && index <= kColor30; }

  void Get(const uint32_t* regs, Constants& out) {
    std::lock_guard lock(mutex_);
    std::memset(&out, 0, sizeof(out));
    out.mode = regs ? regs[kRwMode] & 1 : 0;
    for (uint32_t i = 0; i < 256; ++i) out.entries[i][0] = table_[i];
    if (out.mode)
      for (uint32_t i = 0; i < 128; ++i) std::memcpy(out.entries[i], pwl_[i], 12);
  }

 private:
  static constexpr uint32_t kRwMode = 0x1921, kRwIndex = 0x1922, kSeqColor = 0x1923, kPwlData = 0x1924,
                            kColor30 = 0x1925, kWriteEnMask = 0x1927;

  // After the third component, the next entry.
  void Advance(const uint32_t* regs, uint32_t rw_index) {
    if (++component_ < 3) return;
    component_ = 0;
    const_cast<uint32_t*>(regs)[kRwIndex] = (rw_index + 1) & 0xFF;
  }

  std::mutex mutex_;
  uint32_t component_ = 0;
  uint32_t table_[256];
  uint32_t pwl_[128][3];
};

}  // namespace blitz
