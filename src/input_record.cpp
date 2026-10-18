// Frame clock and input recording.
//
// Frames are counted at the game's D3D present (D3DDevice_Swap, the only caller
// of VdSwap). --input_record=<file> writes every change in player 1's pad state as
// the game sees it (after the XDK XInputGetState wrapper sub_827266E0, polled by
// Harmonix's joypad thread), one line per change, keyed by frame. The format is
// the one --input_replay reads (see input_tape.h).

#include "diagnostics.h"
#include "input_tape.h"
#include "renderer/d3d_hooks.h"

#include <atomic>
#include <cstdio>
#include <mutex>
#include <string>

#include <rex/cvar.h>
#include <rex/hook.h>
#include <rex/input/input.h>
#include <rex/logging.h>
#include <rex/system/kernel_state.h>

REXCVAR_DECLARE(bool, native_renderer);

REXCVAR_DEFINE_STRING(input_record, "", "Blitz",
                      "Record player 1's pad state per frame to this file");

REX_EXTERN(__imp__sub_827266E0);

namespace {
std::atomic<uint32_t> g_frame{0};

struct Recorder {
  std::mutex mutex;
  FILE* file = nullptr;
  bool failed = false;
  blitz::PadState last{};
  bool have_last = false;
};
Recorder g_recorder;
}  // namespace

namespace blitz {
uint32_t CurrentFrame() { return g_frame.load(std::memory_order_relaxed); }
}  // namespace blitz

// D3D present (D3DDevice_Swap): one call per game frame.
namespace {
class FrameClock final : public blitz::d3d::Listener {
 public:
  FrameClock() { blitz::d3d::AddListener(this); }
  void Before(uint32_t function, PPCContext& ctx, uint8_t* base) override {
    if (function == blitz::d3d::kSwap) blitz::WatchSwap(true);
  }
  void After(uint32_t function, PPCContext& ctx, uint8_t* base) override {
    if (function == blitz::d3d::kDrawIndexedVertices || function == blitz::d3d::kDrawVertices) ++draws_;
    if (function != blitz::d3d::kSwap) return;
    blitz::WatchSwap(false);
    uint32_t frame = g_frame.fetch_add(1, std::memory_order_relaxed) + 1;
    // Busy frames (3D scenes) start and end: lines up runs whose loads take different times.
    const bool busy = draws_ > 200;
    if (busy != busy_) REXLOG_INFO("frame_clock: frame {} {} ({} draws)", frame, busy ? "busy" : "quiet", draws_);
    if (busy && !seen_busy_) {
      blitz::RebaseInputReplay(frame);
      blitz::OnFirstBusyFrame(frame);
    }
    seen_busy_ |= busy;
    busy_ = busy;
    draws_ = 0;
    blitz::DumpMemoryAtFrame(REX_KERNEL_STATE()->memory(), frame);
    // The native renderer presents inside the swap; the stock GPU later (SDK patch 8).
    if (REXCVAR_GET(native_renderer)) blitz::ScreenshotAtFrame(frame);
  }

 private:
  uint32_t draws_ = 0;
  bool busy_ = false, seen_busy_ = false;
};
FrameClock g_frame_clock;
}  // namespace

// XInputGetState(user, X_INPUT_STATE*).
REX_HOOK_RAW(sub_827266E0) {
  const uint32_t user = ctx.r3.u32;
  const uint32_t state_ptr = ctx.r4.u32;
  __imp__sub_827266E0(ctx, base);

  static const std::string path = REXCVAR_GET(input_record);
  if (path.empty() || user != 0 || ctx.r3.u32 != 0 || !state_ptr) return;

  const auto& pad = reinterpret_cast<const rex::input::X_INPUT_STATE*>(base + state_ptr)->gamepad;
  blitz::PadState now{uint16_t(pad.buttons), pad.left_trigger, pad.right_trigger,
                    int16_t(pad.thumb_lx), int16_t(pad.thumb_ly),
                    int16_t(pad.thumb_rx), int16_t(pad.thumb_ry)};
  std::lock_guard lock(g_recorder.mutex);
  if (g_recorder.failed || (g_recorder.have_last && now == g_recorder.last)) return;
  if (!g_recorder.file) {
    g_recorder.file = std::fopen(path.c_str(), "w");
    if (!g_recorder.file) {
      g_recorder.failed = true;
      REXLOG_ERROR("input_record: cannot write {}", path);
      return;
    }
    std::fputs("# blitz input tape: <frame> <buttons> [lt rt lx ly rx ry]\n", g_recorder.file);
    REXLOG_INFO("input_record: recording to {}", path);
  }
  g_recorder.last = now;
  g_recorder.have_last = true;
  std::fprintf(g_recorder.file, "%s\n", blitz::FormatTapeLine(blitz::CurrentFrame(), now).c_str());
  std::fflush(g_recorder.file);  // keep the tape if the game crashes
}
