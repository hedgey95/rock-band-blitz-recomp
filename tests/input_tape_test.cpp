// Self-check for the input tape parser and formatter. Build target: blitz_tests.
#undef NDEBUG
#include <cassert>
#include <cstdio>

#include "../src/input_tape.h"

void RunVertexInputTests();  // vertex_input_test.cpp

int main() {
  std::string err;
  auto steps = blitz::ParseInputTape(
      "# boot\n"
      "20 A\r\n"
      "25   -  # release\n"
      "\n"
      "26 START+UP+LT\n"
      "27 RS 0 128 -32768 32767 5 -5\n",
      &err);
  assert(steps && steps->size() == 4);
  assert((*steps)[0].frame == 20 && (*steps)[0].pad.buttons == 0x1000);
  assert((*steps)[1].frame == 25 && (*steps)[1].pad.buttons == 0);
  assert((*steps)[2].pad.buttons == (0x0010 | 0x0001) && (*steps)[2].pad.left_trigger == 0xFF);
  const blitz::PadState& p = (*steps)[3].pad;
  assert(p.buttons == 0x0080 && p.right_trigger == 128 && p.thumb_lx == -32768 &&
         p.thumb_ly == 32767 && p.thumb_rx == 5 && p.thumb_ry == -5);

  // Round trip: what the recorder writes, the player reads back unchanged.
  for (const auto& step : *steps) {
    std::string line = blitz::FormatTapeLine(7, step.pad);
    auto back = blitz::ParseInputTape(line, &err);
    assert(back && back->size() == 1 && (*back)[0].frame == 7 && (*back)[0].pad == step.pad);
  }
  assert(blitz::FormatTapeLine(3, {}) == "3 -");
  assert(blitz::FormatTapeLine(4, {0x1001}) == "4 UP+A");

  assert(!blitz::ParseInputTape("5 A\n4 B\n", &err) && err == "line 2: frame goes backwards");
  assert(!blitz::ParseInputTape("1 JUMP\n", &err) && err.rfind("line 1: unknown button", 0) == 0);
  assert(!blitz::ParseInputTape("A 1\n", &err) && err == "line 1: bad frame");
  assert(!blitz::ParseInputTape("2.5 A\n", &err) && err == "line 1: bad frame");
  assert(!blitz::ParseInputTape("3\n", &err) && err == "line 1: expected '<frame> <buttons>'");
  assert(!blitz::ParseInputTape("3 A 1 2 3\n", &err) &&
         err == "line 1: expected 6 analog values: lt rt lx ly rx ry");
  assert(!blitz::ParseInputTape("3 A 256 0 0 0 0 0\n", &err));
  assert(blitz::ParseInputTape("", &err)->empty());

  std::puts("input_tape_test: OK");
  RunVertexInputTests();
  std::puts("vertex_input_test: OK");
  return 0;
}
