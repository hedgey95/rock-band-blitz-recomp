// Input tapes: per-frame pad states recorded from what the game reads
// (--input_record) and played back into the SDK's input system (--input_replay,
// works without window focus). Frames are game presents since boot.
//
// Format, one step per line ('#' starts a comment):
//   <frame> <BUTTON>[+<BUTTON>...] [lt rt lx ly rx ry]   hold this state from <frame> on
//   <frame> - [lt rt lx ly rx ry]                        no buttons
// Buttons: A B X Y START BACK UP DOWN LEFT RIGHT LB RB LS RS GUIDE, plus LT/RT as
// shorthand for a fully pulled trigger. The optional numbers are the trigger
// (0-255) and stick (-32768..32767) values. Frames must not decrease.

#pragma once

#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace rex::input {
class InputDriver;
}

namespace blitz {

// One pad's state as the game sees it (X_INPUT_GAMEPAD without byte order).
struct PadState {
  uint16_t buttons = 0;  // X_INPUT_GAMEPAD_* bits
  uint8_t left_trigger = 0;
  uint8_t right_trigger = 0;
  int16_t thumb_lx = 0;
  int16_t thumb_ly = 0;
  int16_t thumb_rx = 0;
  int16_t thumb_ry = 0;
  bool operator==(const PadState&) const = default;
};

struct TapeStep {
  uint32_t frame = 0;
  PadState pad;
};

// Returns nullopt and fills *error ("line N: ...") on the first bad line.
std::optional<std::vector<TapeStep>> ParseInputTape(std::string_view text, std::string* error);

// One tape line for `pad` at `frame`; ParseInputTape reads it back unchanged.
std::string FormatTapeLine(uint32_t frame, const PadState& pad);

// Driver for --input_replay=<file>; nullptr when the cvar is empty or the file is bad.
std::unique_ptr<rex::input::InputDriver> CreateReplayInputDriver();

// Called at the run's first busy (3D) frame: with --input_replay_busy_frame, replays
// the rest of the tape relative to it (song inputs follow the song, not the boot).
void RebaseInputReplay(uint32_t first_busy_frame);

// Game frames presented so far (counted at the D3D present).
uint32_t CurrentFrame();

}  // namespace blitz
