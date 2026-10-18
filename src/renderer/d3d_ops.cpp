// Native renderer phase N4: D3D operations other than draws, decoded from the
// game's call arguments. See d3d_ops.h.
//
// Arguments beyond r10 are in the caller's parameter area: 8-byte slots from
// r1+16, a 32-bit value in the low word, so slot n is at r1 + 20 + 8n.
//
// --trace_ops=<file> --trace_ops_frames=<frame,...> logs each decoded op with
// the command buffer range its call wrote, for tools/ops_check.py to compare
// against Xenia's registers for the same draw (SDK patch 6, --trace_draws).

#include "d3d_ops.h"

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <mutex>
#include <string>
#include <vector>

#include <rex/cvar.h>
#include <rex/logging.h>
#include <rex/memory/utils.h>

#include "../input_tape.h"
#include "d3d_hooks.h"

REXCVAR_DEFINE_STRING(trace_ops, "", "Blitz", "Dev: log decoded D3D resolves and clears");
REXCVAR_DEFINE_STRING(trace_ops_frames, "", "Blitz", "Dev: comma-separated frames for --trace_ops");

namespace blitz::d3d {

namespace {

uint32_t Load(const uint8_t* base, uint32_t address) {
  return rex::memory::load_and_swap<uint32_t>(base + address);
}

float LoadFloat(const uint8_t* base, uint32_t address) {
  uint32_t bits = Load(base, address);
  float value;
  std::memcpy(&value, &bits, 4);
  return value;
}

uint32_t StackArg(const PPCContext& ctx, const uint8_t* base, uint32_t slot) {
  return Load(base, ctx.r1.u32 + 20 + 8 * slot);
}

// Texture objects keep virtual addresses; D3D sends the GPU physical ones
// (0xE0000000+ addresses are offset by 4 KB).
uint32_t Physical(uint32_t address) {
  return (address & 0x1FFFFFFF) + ((((address >> 20) & 0xFFF) + 0x200) & 0x1000);
}

bool LoadRect(const uint8_t* base, uint32_t address, int32_t (&rect)[4]) {
  if (!address) return false;
  for (int i = 0; i < 4; ++i) rect[i] = int32_t(Load(base, address + 4 * i));
  return true;
}

}  // namespace

ResolveOp DecodeResolve(const PPCContext& ctx, const uint8_t* base) {
  ResolveOp op{};
  op.flags = ctx.r4.u32;
  op.source = op.flags & 7;
  op.clear_color = op.flags & 0x100;
  op.clear_depth = op.flags & 0x200;
  op.has_rect = LoadRect(base, ctx.r5.u32, op.rect);
  op.dest_texture = ctx.r8.u32;
  if (op.dest_texture) {
    for (int i = 0; i < 6; ++i) op.dest_fetch[i] = Load(base, op.dest_texture + 28 + 4 * i);
    // As the GPU sees it: physical base (dword 1) and mip (dword 5) addresses.
    op.dest_fetch[1] = (op.dest_fetch[1] & 0xFFF) | Physical(op.dest_fetch[1] & 0xFFFFF000);
    if (op.dest_fetch[5] & 0xFFFFF000)
      op.dest_fetch[5] = (op.dest_fetch[5] & 0xFFF) | Physical(op.dest_fetch[5] & 0xFFFFF000);
  }
  if (ctx.r9.u32) {
    op.dest_point[0] = int32_t(Load(base, ctx.r9.u32));
    op.dest_point[1] = int32_t(Load(base, ctx.r9.u32 + 4));
  }
  op.dest_level = ctx.r10.u32;
  op.dest_slice = StackArg(ctx, base, 8);
  if (uint32_t color = StackArg(ctx, base, 9))
    for (int i = 0; i < 4; ++i) op.clear_color_value[i] = LoadFloat(base, color + 4 * i);
  op.clear_z = float(ctx.f1.f64);
  op.clear_stencil = StackArg(ctx, base, 11);
  return op;
}

ClearOp DecodeClear(const PPCContext& ctx, const uint8_t* base) {
  ClearOp op{};
  op.flags = ctx.r4.u32;
  op.has_rect = LoadRect(base, ctx.r5.u32, op.rect);
  if (ctx.r6.u32)
    for (int i = 0; i < 4; ++i) op.color[i] = LoadFloat(base, ctx.r6.u32 + 4 * i);
  op.z = float(ctx.f1.f64);
  op.stencil = ctx.r8.u32;  // r7's slot is taken by Z
  return op;
}

namespace {

// Dev log: magic 'OPRS', frame, function, command pointer before and after,
// device registers 0x2000-0x2002 at the call, then the op's raw bytes.
class OpsTrace final : public Listener {
 public:
  OpsTrace() { AddListener(this); }

  void Before(uint32_t function, PPCContext& ctx, uint8_t* base) override {
    if (function != kResolve && function != kClearF) return;
    if (!Wanted(blitz::CurrentFrame())) return;
    Pending pending{function, ctx.r3.u32, Load(base, ctx.r3.u32 + kDeviceCommandPtr)};
    if (function == kResolve) pending.resolve = DecodeResolve(ctx, base);
    else pending.clear = DecodeClear(ctx, base);
    for (int i = 0; i < 3; ++i)
      pending.surface[i] = Load(base, ctx.r3.u32 + kDeviceRegisters2000 + 4 * i);
    t_pending.push_back(pending);
  }

  void After(uint32_t function, PPCContext& ctx, uint8_t* base) override {
    if ((function != kResolve && function != kClearF) || t_pending.empty() ||
        t_pending.back().function != function)
      return;
    const Pending p = t_pending.back();
    t_pending.pop_back();
    const uint32_t head[] = {0x5352504F, blitz::CurrentFrame(), function, p.before,
                             Load(base, p.device + kDeviceCommandPtr), p.surface[0], p.surface[1],
                             p.surface[2]};
    std::lock_guard lock(mutex_);
    std::fwrite(head, sizeof(head), 1, file_);
    if (function == kResolve) std::fwrite(&p.resolve, sizeof(p.resolve), 1, file_);
    else std::fwrite(&p.clear, sizeof(p.clear), 1, file_);
    std::fflush(file_);
  }

 private:
  struct Pending {
    uint32_t function, device, before;
    uint32_t surface[3];
    ResolveOp resolve{};
    ClearOp clear{};
  };

  bool Wanted(uint32_t frame) {
    std::call_once(init_, [this] {
      std::string list = REXCVAR_GET(trace_ops_frames);
      for (size_t pos = 0; pos < list.size();) {
        size_t end = std::min(list.find(',', pos), list.size());
        frames_.push_back(uint32_t(std::stoul(list.substr(pos, end - pos))));
        pos = end + 1;
      }
      const std::string path = REXCVAR_GET(trace_ops);
      if (!path.empty() && !(file_ = std::fopen(path.c_str(), "wb")))
        REXLOG_ERROR("trace_ops: cannot write {}", path);
    });
    return file_ && std::find(frames_.begin(), frames_.end(), frame) != frames_.end();
  }

  std::once_flag init_;
  std::mutex mutex_;
  FILE* file_ = nullptr;
  std::vector<uint32_t> frames_;
  static thread_local std::vector<Pending> t_pending;
};

thread_local std::vector<OpsTrace::Pending> OpsTrace::t_pending;

OpsTrace g_ops_trace;

}  // namespace

}  // namespace blitz::d3d
