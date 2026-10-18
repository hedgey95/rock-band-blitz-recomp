// Parser and formatter for input tapes; no SDK dependencies so tests can build it alone.

#include "input_tape.h"

#include <charconv>
#include <utility>

namespace blitz {

namespace {

struct ButtonName {
  std::string_view name;
  uint16_t bit;
};

// Values match X_INPUT_GAMEPAD_* in <rex/input/input.h>.
constexpr ButtonName kButtons[] = {
    {"UP", 0x0001},    {"DOWN", 0x0002}, {"LEFT", 0x0004}, {"RIGHT", 0x0008},
    {"START", 0x0010}, {"BACK", 0x0020}, {"LS", 0x0040},   {"RS", 0x0080},
    {"LB", 0x0100},    {"RB", 0x0200},   {"A", 0x1000},    {"B", 0x2000},
    {"X", 0x4000},     {"Y", 0x8000},     {"GUIDE", 0x0400},
};

std::string_view Trim(std::string_view s) {
  while (!s.empty() && (s.front() == ' ' || s.front() == '\t' || s.front() == '\r')) s.remove_prefix(1);
  while (!s.empty() && (s.back() == ' ' || s.back() == '\t' || s.back() == '\r')) s.remove_suffix(1);
  return s;
}

// Splits off the next whitespace-separated token.
std::string_view NextToken(std::string_view& s) {
  s = Trim(s);
  size_t end = s.find_first_of(" \t");
  std::string_view token = s.substr(0, end);
  s = end == std::string_view::npos ? std::string_view{} : s.substr(end);
  return token;
}

bool ParseButtons(std::string_view spec, PadState& pad) {
  if (spec == "-") return true;
  while (!spec.empty()) {
    size_t plus = spec.find('+');
    std::string_view token = spec.substr(0, plus);
    spec = plus == std::string_view::npos ? std::string_view{} : spec.substr(plus + 1);
    if (token == "LT") {
      pad.left_trigger = 0xFF;
    } else if (token == "RT") {
      pad.right_trigger = 0xFF;
    } else {
      bool found = false;
      for (const auto& b : kButtons) {
        if (b.name == token) {
          pad.buttons |= b.bit;
          found = true;
        }
      }
      if (!found) return false;
    }
  }
  return true;
}

template <typename T>
bool ParseInt(std::string_view text, int lo, int hi, T& out) {
  int v = 0;
  auto [end, ec] = std::from_chars(text.data(), text.data() + text.size(), v);
  if (ec != std::errc() || end != text.data() + text.size() || v < lo || v > hi) return false;
  out = static_cast<T>(v);
  return true;
}

}  // namespace

std::optional<std::vector<TapeStep>> ParseInputTape(std::string_view text, std::string* error) {
  std::vector<TapeStep> steps;
  int line_no = 0;
  while (!text.empty()) {
    ++line_no;
    size_t nl = text.find('\n');
    std::string_view line = text.substr(0, nl);
    text = nl == std::string_view::npos ? std::string_view{} : text.substr(nl + 1);
    if (size_t hash = line.find('#'); hash != std::string_view::npos) line = line.substr(0, hash);
    line = Trim(line);
    if (line.empty()) continue;

    auto fail = [&](std::string_view why) {
      if (error) *error = "line " + std::to_string(line_no) + ": " + std::string(why);
      return std::nullopt;
    };
    std::string_view frame_text = NextToken(line);
    std::string_view spec = NextToken(line);
    if (spec.empty()) return fail("expected '<frame> <buttons>'");

    TapeStep step;
    auto [end, ec] = std::from_chars(frame_text.data(), frame_text.data() + frame_text.size(), step.frame);
    if (ec != std::errc() || end != frame_text.data() + frame_text.size()) return fail("bad frame");
    if (!steps.empty() && step.frame < steps.back().frame) return fail("frame goes backwards");
    if (!ParseButtons(spec, step.pad)) return fail("unknown button in '" + std::string(spec) + "'");

    if (!Trim(line).empty()) {
      PadState& p = step.pad;
      std::string_view f[6];
      for (auto& t : f) t = NextToken(line);
      if (!Trim(line).empty() || f[5].empty() || !ParseInt(f[0], 0, 255, p.left_trigger) ||
          !ParseInt(f[1], 0, 255, p.right_trigger) || !ParseInt(f[2], -32768, 32767, p.thumb_lx) ||
          !ParseInt(f[3], -32768, 32767, p.thumb_ly) || !ParseInt(f[4], -32768, 32767, p.thumb_rx) ||
          !ParseInt(f[5], -32768, 32767, p.thumb_ry))
        return fail("expected 6 analog values: lt rt lx ly rx ry");
    }
    steps.push_back(step);
  }
  return steps;
}

std::string FormatTapeLine(uint32_t frame, const PadState& pad) {
  std::string line = std::to_string(frame) + ' ';
  std::string names;
  for (const auto& b : kButtons) {
    if (pad.buttons & b.bit) names += (names.empty() ? "" : "+") + std::string(b.name);
  }
  line += names.empty() ? "-" : names;
  if (pad.left_trigger || pad.right_trigger || pad.thumb_lx || pad.thumb_ly || pad.thumb_rx ||
      pad.thumb_ry) {
    for (int v : {int(pad.left_trigger), int(pad.right_trigger), int(pad.thumb_lx),
                  int(pad.thumb_ly), int(pad.thumb_rx), int(pad.thumb_ry)})
      line += ' ' + std::to_string(v);
  }
  return line;
}

}  // namespace blitz
