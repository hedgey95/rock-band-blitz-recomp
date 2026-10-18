// Replay driver: a synthetic pad (merged into guest user 0 with any real pad)
// whose state follows the steps of --input_replay, keyed by game frame.

#include "input_tape.h"

#include <algorithm>
#include <atomic>
#include <cstring>
#include <fstream>
#include <sstream>

#include <rex/cvar.h>
#include <rex/input/input.h>
#include <rex/input/input_driver.h>
#include <rex/logging.h>

REXCVAR_DEFINE_STRING(input_replay, "", "Blitz",
                      "Input tape to play back, keyed by game frame (see src/input_tape.h)");
REXCVAR_DEFINE_UINT32(input_replay_busy_frame, 0, "Blitz",
                      "First busy (3D) frame of the run the tape was recorded in: steps from the "
                      "first busy frame on are played that far from this run's first busy frame");

namespace blitz {

namespace {

// This run's first busy frame minus --input_replay_busy_frame; 0 until the first busy frame.
std::atomic<int32_t> g_busy_shift{0};

using rex::X_RESULT;
using rex::X_STATUS;
using namespace rex::input;

constexpr DeviceId kReplayDevice = static_cast<DeviceId>(0x53435249);  // "SCRI"

class ReplayInputDriver final : public InputDriver {
 public:
  explicit ReplayInputDriver(std::vector<TapeStep> steps)
      : InputDriver(nullptr, 0), steps_(std::move(steps)) {}

  X_STATUS Setup() override {
    return X_STATUS_SUCCESS;
  }

  void EnumerateDevices(std::vector<DeviceInfo>& out) override {
    DeviceInfo info;
    info.id = kReplayDevice;
    info.name = "Input replay";
    info.synthetic = true;
    out.push_back(info);
  }

  X_RESULT GetDeviceState(DeviceId id, X_INPUT_STATE* out_state) override {
    if (id != kReplayDevice) return X_ERROR_DEVICE_NOT_CONNECTED;
    // The game's joypad thread polls between presents; the song starts at the first busy frame.
    const uint32_t frame = CurrentFrame() - g_busy_shift.load(std::memory_order_relaxed);
    // Index of the step in effect: the last one whose time has passed (0 = before the first).
    size_t current = std::upper_bound(steps_.begin(), steps_.end(), frame,
                                      [](uint32_t f, const TapeStep& s) { return f < s.frame; }) -
                     steps_.begin();
    size_t logged = logged_.load();
    if (current > logged && logged_.compare_exchange_strong(logged, current)) {
      REXLOG_DEBUG("input_replay: frame {} step {}/{} buttons={:04X}", frame, current,
                   steps_.size(), steps_[current - 1].pad.buttons);
    }
    if (out_state) {
      std::memset(out_state, 0, sizeof(*out_state));
      out_state->packet_number = static_cast<uint32_t>(current);
      if (current > 0) {
        const PadState& p = steps_[current - 1].pad;
        out_state->gamepad.buttons = p.buttons;
        out_state->gamepad.left_trigger = p.left_trigger;
        out_state->gamepad.right_trigger = p.right_trigger;
        out_state->gamepad.thumb_lx = p.thumb_lx;
        out_state->gamepad.thumb_ly = p.thumb_ly;
        out_state->gamepad.thumb_rx = p.thumb_rx;
        out_state->gamepad.thumb_ry = p.thumb_ry;
      }
    }
    return X_ERROR_SUCCESS;
  }

  X_RESULT GetDeviceCapabilities(DeviceId id, uint32_t, X_INPUT_CAPABILITIES* out_caps) override {
    if (id != kReplayDevice) return X_ERROR_DEVICE_NOT_CONNECTED;
    if (out_caps) {
      std::memset(out_caps, 0, sizeof(*out_caps));
      out_caps->type = 0x01;      // XINPUT_DEVTYPE_GAMEPAD
      out_caps->sub_type = 0x01;  // XINPUT_DEVSUBTYPE_GAMEPAD
      out_caps->gamepad.buttons = 0xFFFF;
      out_caps->gamepad.left_trigger = 0xFF;
      out_caps->gamepad.right_trigger = 0xFF;
    }
    return X_ERROR_SUCCESS;
  }

  X_RESULT SetDeviceVibration(DeviceId id, X_INPUT_VIBRATION*) override {
    return id == kReplayDevice ? X_ERROR_SUCCESS : X_ERROR_DEVICE_NOT_CONNECTED;
  }

  X_RESULT GetDeviceKeystroke(DeviceId id, uint32_t, X_INPUT_KEYSTROKE*) override {
    return id == kReplayDevice ? X_ERROR_EMPTY : X_ERROR_DEVICE_NOT_CONNECTED;
  }

 private:
  std::vector<TapeStep> steps_;
  std::atomic<size_t> logged_{0};
};

}  // namespace

void RebaseInputReplay(uint32_t first_busy_frame) {
  const uint32_t recorded = REXCVAR_GET(input_replay_busy_frame);
  if (REXCVAR_GET(input_replay).empty() || !recorded) return;
  g_busy_shift.store(int32_t(first_busy_frame - recorded), std::memory_order_relaxed);
  REXLOG_INFO("input_replay: first busy frame {} (tape {}); later steps move {} frames",
              first_busy_frame, recorded, int32_t(first_busy_frame - recorded));
}

std::unique_ptr<InputDriver> CreateReplayInputDriver() {
  const std::string path = REXCVAR_GET(input_replay);
  if (path.empty()) return nullptr;
  std::ifstream file(path, std::ios::binary);
  if (!file) {
    REXLOG_ERROR("input_replay: cannot open {}", path);
    return nullptr;
  }
  std::stringstream text;
  text << file.rdbuf();
  std::string error;
  auto steps = ParseInputTape(text.str(), &error);
  if (!steps) {
    REXLOG_ERROR("input_replay: {}: {}", path, error);
    return nullptr;
  }
  REXLOG_INFO("input_replay: {} steps from {}", steps->size(), path);
  auto driver = std::make_unique<ReplayInputDriver>(std::move(*steps));
  if (driver->Setup() != X_STATUS_SUCCESS) return nullptr;
  return driver;
}

}  // namespace blitz
