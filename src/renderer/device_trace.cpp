// Dev aid for native-renderer phase N2: records which XDK D3D call wrote each
// part of the command buffer, with a snapshot of the D3D device after each
// outermost call, so the device's shadow state can be checked draw by draw
// against the register file Xenia decodes (SDK patch 6, --trace_draws).
//
//   --trace_device=<file> --trace_device_frames=<frame,...>
//
// One record per hooked D3D call (d3d_hooks.cpp) that moved the command
// pointer (device+48): magic 'CDRW', frame, function address, nesting depth
// (bit 31: a snapshot follows), r3, r4 (64-bit, high word first: the flush
// helpers take 64-bit dirty masks), r5..r10 at entry, pointer before, pointer
// after. Outermost calls (depth 0) and the game's draw calls are followed by
// 16 KB of the device (guest memory, big-endian) as it is after the call, then
// 32 bytes of the bound index buffer object. Draw wrappers restore temporary
// state afterwards, so the inner snapshot is the one that matches the draw.

#include <algorithm>
#include <atomic>
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

REXCVAR_DEFINE_STRING(trace_device, "", "Blitz", "Dev: log D3D command writes and device snapshots");
REXCVAR_DEFINE_STRING(trace_device_frames, "", "Blitz",
                      "Dev: comma-separated frames for --trace_device");

namespace {

namespace d3d = blitz::d3d;

constexpr uint32_t kDeviceBytes = 16384;
constexpr uint32_t kIndexBufferBytes = 32;

struct Call {
  bool active;
  uint32_t depth;
  uint32_t before;
  uint32_t args[9];
};

class DeviceTrace final : public d3d::Listener {
 public:
  DeviceTrace() { d3d::AddListener(this); }

  void Before(uint32_t function, PPCContext& ctx, uint8_t* base) override {
    const bool api = IsApiDraw(function);
    // Only the thread that issues draws writes commands; other threads' calls
    // would see its pointer moves as their own.
    if (api && !t_render_thread) {
      device_.store(ctx.r3.u32);
      t_render_thread = true;
    }
    Call call{};
    call.active = t_render_thread && Wanted(blitz::CurrentFrame());
    if (call.active) {
      const uint32_t args[9] = {ctx.r3.u32, uint32_t(ctx.r4.u64 >> 32), ctx.r4.u32, ctx.r5.u32,
                                ctx.r6.u32, ctx.r7.u32, ctx.r8.u32, ctx.r9.u32, ctx.r10.u32};
      std::memcpy(call.args, args, sizeof(args));
      call.before = CommandPtr(base);
      call.depth = t_depth++;
    }
    t_calls.push_back(call);
  }

  void After(uint32_t function, PPCContext& ctx, uint8_t* base) override {
    const Call call = t_calls.back();
    t_calls.pop_back();
    if (!call.active) return;
    --t_depth;
    const bool api = IsApiDraw(function);
    const uint32_t after = CommandPtr(base);
    if (after != call.before || api)  // Begin*Vertices never moves it
      Record(function, call, call.depth == 0 || api, after, base);
  }

 private:
  static bool IsApiDraw(uint32_t function) {
    // r3 is the device in these (checked in the asm), so they can seed the trace.
    return function == d3d::kDrawVertices || function == d3d::kDrawIndexedVertices ||
           function == d3d::kBeginVertices || function == d3d::kBeginIndexedVertices;
  }

  bool Wanted(uint32_t frame) {
    std::call_once(init_, [this] {
      std::string list = REXCVAR_GET(trace_device_frames);
      for (size_t pos = 0; pos < list.size();) {
        size_t end = std::min(list.find(',', pos), list.size());
        frames_.push_back(uint32_t(std::stoul(list.substr(pos, end - pos))));
        pos = end + 1;
      }
      const std::string path = REXCVAR_GET(trace_device);
      if (!path.empty() && !(file_ = std::fopen(path.c_str(), "wb")))
        REXLOG_ERROR("trace_device: cannot write {}", path);
    });
    return file_ && std::find(frames_.begin(), frames_.end(), frame) != frames_.end();
  }

  uint32_t CommandPtr(uint8_t* base) const {
    return rex::memory::load_and_swap<uint32_t>(base + device_.load() + d3d::kDeviceCommandPtr);
  }

  void Record(uint32_t function, const Call& call, bool snapshot, uint32_t after, uint8_t* base) {
    const uint32_t head[] = {0x57524443, blitz::CurrentFrame(), function,
                             call.depth | (uint32_t(snapshot) << 31),
                             call.args[0], call.args[1], call.args[2], call.args[3], call.args[4],
                             call.args[5], call.args[6], call.args[7], call.args[8],
                             call.before, after};
    std::lock_guard lock(mutex_);
    std::fwrite(head, sizeof(head), 1, file_);
    if (snapshot) {
      const uint32_t device = device_.load();
      std::fwrite(base + device, 1, kDeviceBytes, file_);
      const uint32_t ib =
          rex::memory::load_and_swap<uint32_t>(base + device + d3d::kDeviceIndexBuffer);
      uint8_t ib_bytes[kIndexBufferBytes] = {};
      if (ib) std::memcpy(ib_bytes, base + ib, sizeof(ib_bytes));
      std::fwrite(ib_bytes, 1, sizeof(ib_bytes), file_);
    }
    std::fflush(file_);  // the game is killed, not shut down
  }

  std::once_flag init_;
  std::mutex mutex_;
  FILE* file_ = nullptr;
  std::vector<uint32_t> frames_;
  std::atomic<uint32_t> device_{0};
  static thread_local uint32_t t_depth;
  static thread_local bool t_render_thread;
  static thread_local std::vector<Call> t_calls;
};

thread_local uint32_t DeviceTrace::t_depth = 0;
thread_local bool DeviceTrace::t_render_thread = false;
thread_local std::vector<Call> DeviceTrace::t_calls;

DeviceTrace g_device_trace;

}  // namespace
