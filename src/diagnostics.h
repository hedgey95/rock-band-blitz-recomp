// Dev diagnostics: see diagnostics.cpp.
#pragma once

#include <cstdint>

namespace rex::memory {
class Memory;
}
namespace rex::ui {
class Presenter;
}

namespace blitz {

// Starts the --dump_stacks_at timer; does nothing when the cvar is 0.
void StartStackDumpTimer();

// --dump_stacks_on_stall: the game thread enters (true) or leaves (false) a present.
void WatchSwap(bool entering);

// Starts the --screenshot_at timer: writes the presenter's guest output (the
// game's frame, whether or not the window is shown) as <prefix>-<s>s.bmp.
// Also keeps the presenter for ScreenshotAtFrame.
void StartScreenshotTimer(rex::ui::Presenter* presenter);

// Native renderer: saves the frame just presented when it is one of
// --screenshot_frames (the stock GPU saves its own; SDK patch 8).
void ScreenshotAtFrame(uint32_t frame);

// The first frame with a 3D scene: turns --screenshot_busy_frames into frames.
void OnFirstBusyFrame(uint32_t frame);

// Writes the loaded image when --dump_image is set.
void DumpImageIfRequested(rex::memory::Memory* memory, uint32_t image_base, uint32_t image_size);

// Writes guest physical memory when `frame` is one of --dump_memory_at.
void DumpMemoryAtFrame(rex::memory::Memory* memory, uint32_t frame);

}  // namespace blitz
