// Dev diagnostics.
//
// Hangs: --dump_stacks_at=<seconds> logs every thread's native
// call stack three times, 1 s apart, starting that long after startup (a thread
// stuck in the same place in all three is the hang). Recompiled guest functions show up as
// sub_XXXXXXXX, so the dump reads as guest call stacks plus the SDK frames they wait in.

#include "diagnostics.h"

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <string>
#include <atomic>
#include <mutex>
#include <thread>
#include <utility>
#include <vector>

#include <windows.h>
// windows.h first
#include <dbghelp.h>
#include <tlhelp32.h>

#include <rex/cvar.h>
#include <rex/logging.h>
#include <rex/system/xmemory.h>
#include <rex/ui/presenter.h>

REXCVAR_DEFINE_STRING(dump_image, "", "Blitz",
                      "Write the loaded (decrypted) XEX image to this file, for offline analysis");
REXCVAR_DEFINE_STRING(dump_memory, "", "Blitz",
                      "Write guest physical memory to <file>-<frame>.bin at --dump_memory_at");
REXCVAR_DEFINE_STRING(dump_memory_at, "", "Blitz", "Comma-separated frames for --dump_memory");
REXCVAR_DEFINE_STRING(screenshot_at, "", "Blitz",
                      "Comma-separated seconds after setup at which to save the game's frame");
REXCVAR_DEFINE_STRING(screenshot_prefix, "screenshot", "Blitz",
                      "Path prefix for --screenshot_at files (<prefix>-<s>s.bmp)");
REXCVAR_DEFINE_STRING(screenshot_busy_frames, "", "Blitz",
                      "Dev: like --screenshot_frames, counted from the first busy (3D) frame, so runs "
                      "whose loads take different times line up");
REXCVAR_DEFINE_STRING(screenshot_frames, "", "Blitz",
                      "Dev: comma-separated game frames to save as <prefix>-f<n>.bmp (frame-exact; "
                      "the stock GPU's captures are in SDK patch 8)");
REXCVAR_DEFINE_BOOL(dump_stacks_on_stall, false, "Blitz",
                    "Dev: log all thread stacks (twice per run) when a present is blocked for 25 ms");
REXCVAR_DEFINE_DOUBLE(dump_stacks_at, 0, "Blitz",
                      "Seconds after startup to log all thread stacks 3x, 1 s apart (0 = off)");

namespace blitz {

namespace {

constexpr int kMaxFrames = 40;

// Walks a suspended thread with the OS unwinder only; nothing here allocates or
// takes dbghelp locks the suspended thread might hold.
std::vector<DWORD64> CaptureStack(HANDLE thread) {
  std::vector<DWORD64> pcs;
  pcs.reserve(kMaxFrames);
  CONTEXT ctx{};
  ctx.ContextFlags = CONTEXT_FULL;
  if (!GetThreadContext(thread, &ctx)) return pcs;
  for (int i = 0; i < kMaxFrames && ctx.Rip; ++i) {
    pcs.push_back(ctx.Rip);
    DWORD64 image_base = 0;
    PRUNTIME_FUNCTION fn = RtlLookupFunctionEntry(ctx.Rip, &image_base, nullptr);
    if (!fn) {  // leaf function: return address is at the top of the stack
      ctx.Rip = *reinterpret_cast<DWORD64*>(ctx.Rsp);
      ctx.Rsp += 8;
      continue;
    }
    void* handler_data = nullptr;
    DWORD64 establisher = 0;
    RtlVirtualUnwind(UNW_FLAG_NHANDLER, image_base, ctx.Rip, fn, &ctx, &handler_data,
                     &establisher, nullptr);
  }
  return pcs;
}

std::string ThreadName(HANDLE thread) {
  PWSTR desc = nullptr;
  std::string name;
  if (SUCCEEDED(GetThreadDescription(thread, &desc)) && desc) {
    int n = WideCharToMultiByte(CP_UTF8, 0, desc, -1, nullptr, 0, nullptr, nullptr);
    if (n > 1) {
      name.resize(n - 1);
      WideCharToMultiByte(CP_UTF8, 0, desc, -1, name.data(), n, nullptr, nullptr);
    }
    LocalFree(desc);
  }
  return name;
}

void DumpAllThreadStacks() {
  struct Stack {
    DWORD tid;
    std::string name;
    std::vector<DWORD64> pcs;
  };
  std::vector<Stack> stacks;
  HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPTHREAD, 0);
  if (snap == INVALID_HANDLE_VALUE) return;
  THREADENTRY32 te{};
  te.dwSize = sizeof(te);
  for (BOOL ok = Thread32First(snap, &te); ok; ok = Thread32Next(snap, &te)) {
    if (te.th32OwnerProcessID != GetCurrentProcessId() || te.th32ThreadID == GetCurrentThreadId())
      continue;
    HANDLE th = OpenThread(THREAD_SUSPEND_RESUME | THREAD_GET_CONTEXT | THREAD_QUERY_LIMITED_INFORMATION,
                           FALSE, te.th32ThreadID);
    if (!th) continue;
    Stack s{te.th32ThreadID, ThreadName(th), {}};
    if (SuspendThread(th) != static_cast<DWORD>(-1)) {
      s.pcs = CaptureStack(th);
      ResumeThread(th);
    }
    CloseHandle(th);
    stacks.push_back(std::move(s));
  }
  CloseHandle(snap);

  HANDLE process = GetCurrentProcess();
  SymSetOptions(SYMOPT_UNDNAME | SYMOPT_DEFERRED_LOADS);
  SymInitialize(process, nullptr, TRUE);
  alignas(SYMBOL_INFO) char buf[sizeof(SYMBOL_INFO) + 256];
  auto* sym = reinterpret_cast<SYMBOL_INFO*>(buf);
  REXLOG_INFO("dump_stacks: snapshot of {} threads", stacks.size());
  for (const auto& s : stacks) {
    REXLOG_INFO("dump_stacks: thread {} '{}'", s.tid, s.name);
    for (DWORD64 pc : s.pcs) {
      sym->SizeOfStruct = sizeof(SYMBOL_INFO);
      sym->MaxNameLen = 255;
      DWORD64 disp = 0;
      if (SymFromAddr(process, pc, &disp, sym))
        REXLOG_INFO("dump_stacks:   {}+0x{:X}", sym->Name, disp);
      else
        REXLOG_INFO("dump_stacks:   0x{:X}", pc);
    }
  }
  SymCleanup(process);
}

}  // namespace

void WatchSwap(bool entering) {
  if (!REXCVAR_GET(dump_stacks_on_stall)) return;
  static std::atomic<int64_t> entered_ns{0};
  static std::once_flag started;
  std::call_once(started, [] {
    std::thread([] {
      int dumps = 0;
      while (dumps < 2) {
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
        const int64_t t = entered_ns.load();
        const int64_t now = std::chrono::steady_clock::now().time_since_epoch().count();
        if (t && now - t > 25'000'000) {
          REXLOG_WARN("dump_stacks_on_stall: a present has been blocked for 25 ms");
          DumpAllThreadStacks();
          ++dumps;
          while (entered_ns.load() == t) std::this_thread::sleep_for(std::chrono::milliseconds(5));
        }
      }
    }).detach();
  });
  entered_ns = entering ? std::chrono::steady_clock::now().time_since_epoch().count() : 0;
}

void StartStackDumpTimer() {
  double at = REXCVAR_GET(dump_stacks_at);
  if (at <= 0) return;
  std::thread([at] {
    std::this_thread::sleep_for(std::chrono::duration<double>(at));
    for (int i = 0; i < 3; ++i) {
      if (i) std::this_thread::sleep_for(std::chrono::seconds(1));
      DumpAllThreadStacks();
    }
  }).detach();
}

namespace {

// 32-bit top-down BMP from the presenter's R8 G8 B8 X8 rows.
bool WriteBmp(const std::string& path, const rex::ui::RawImage& image) {
  FILE* f = std::fopen(path.c_str(), "wb");
  if (!f) return false;
  const uint32_t size = image.width * image.height * 4;
  uint8_t header[54] = {'B', 'M'};
  auto put = [&](int at, uint32_t v) { std::memcpy(header + at, &v, 4); };
  put(2, 54 + size);
  put(10, 54);
  put(14, 40);
  put(18, image.width);
  put(22, uint32_t(-int32_t(image.height)));  // negative height: top-down rows
  header[26] = 1;
  header[28] = 32;
  put(34, size);
  std::fwrite(header, 1, sizeof(header), f);
  std::vector<uint8_t> row(image.width * 4);
  for (uint32_t y = 0; y < image.height; ++y) {
    const uint8_t* src = image.data.data() + y * image.stride;
    for (uint32_t x = 0; x < image.width; ++x) {
      row[4 * x] = src[4 * x + 2];
      row[4 * x + 1] = src[4 * x + 1];
      row[4 * x + 2] = src[4 * x];
      row[4 * x + 3] = 255;
    }
    std::fwrite(row.data(), 1, row.size(), f);
  }
  std::fclose(f);
  return true;
}

}  // namespace

namespace {
rex::ui::Presenter* g_presenter = nullptr;
}  // namespace

void ScreenshotAtFrame(uint32_t frame) {
  // Re-read when it changes: --screenshot_busy_frames sets it during the run.
  static std::string list;
  static std::vector<uint32_t> frames;
  if (REXCVAR_GET(screenshot_frames) != list) {
    list = REXCVAR_GET(screenshot_frames);
    frames.clear();
    for (size_t pos = 0; pos < list.size();) {
      size_t end = std::min(list.find(',', pos), list.size());
      frames.push_back(uint32_t(std::strtoul(list.substr(pos, end - pos).c_str(), nullptr, 10)));
      pos = end + 1;
    }
  }
  if (!g_presenter || std::find(frames.begin(), frames.end(), frame) == frames.end()) return;
  rex::ui::RawImage image;
  const std::string path = REXCVAR_GET(screenshot_prefix) + "-f" + std::to_string(frame) + ".bmp";
  if (!g_presenter->CaptureGuestOutput(image) || !WriteBmp(path, image))
    REXLOG_ERROR("screenshot_frames: no capture of frame {}", frame);
}

void OnFirstBusyFrame(uint32_t frame) {
  const std::string offsets = REXCVAR_GET(screenshot_busy_frames);
  std::string list = REXCVAR_GET(screenshot_frames);
  for (size_t pos = 0; pos < offsets.size();) {
    size_t end = std::min(offsets.find(',', pos), offsets.size());
    const uint32_t at = frame + uint32_t(std::strtoul(offsets.substr(pos, end - pos).c_str(), nullptr, 10));
    list += (list.empty() ? "" : ",") + std::to_string(at);
    pos = end + 1;
  }
  if (!offsets.empty()) {
    rex::cvar::SetFlagByName("screenshot_frames", list);
    REXLOG_INFO("screenshot_busy_frames: first busy frame {}; capturing {}", frame, list);
  }
}

void StartScreenshotTimer(rex::ui::Presenter* presenter) {
  g_presenter = presenter;
  std::string list = REXCVAR_GET(screenshot_at);
  if (list.empty()) return;
  if (!presenter) {
    REXLOG_ERROR("screenshot_at: no presenter");
    return;
  }
  std::vector<double> times;
  for (size_t pos = 0; pos < list.size();) {
    size_t end = std::min(list.find(',', pos), list.size());
    times.push_back(std::stod(list.substr(pos, end - pos)));
    pos = end + 1;
  }
  std::sort(times.begin(), times.end());
  std::thread([presenter, times, prefix = REXCVAR_GET(screenshot_prefix)] {
    const auto start = std::chrono::steady_clock::now();
    for (double t : times) {
      std::this_thread::sleep_until(start + std::chrono::duration_cast<std::chrono::steady_clock::duration>(
                                                std::chrono::duration<double>(t)));
      rex::ui::RawImage image;
      char suffix[32];
      std::snprintf(suffix, sizeof(suffix), "-%gs.bmp", t);
      if (!presenter->CaptureGuestOutput(image) || !WriteBmp(prefix + suffix, image))
        REXLOG_ERROR("screenshot_at: no frame at {}s", t);
    }
  }).detach();
}

// Image analysis: --dump_image=<file> writes guest memory [image_base, +image_size)
// right after load, so strings and data can be searched offline. Keep it outside the repo.
void DumpImageIfRequested(rex::memory::Memory* memory, uint32_t image_base, uint32_t image_size) {
  const std::string path = REXCVAR_GET(dump_image);
  if (path.empty()) return;
  FILE* f = std::fopen(path.c_str(), "wb");
  if (!f) {
    REXLOG_ERROR("dump_image: cannot write {}", path);
    return;
  }
  size_t written = std::fwrite(memory->TranslateVirtual<const uint8_t*>(image_base), 1, image_size, f);
  std::fclose(f);
  REXLOG_INFO("dump_image: {} bytes at 0x{:08X} -> {}", written, image_base, path);
}

// State analysis: --dump_memory=<prefix> --dump_memory_at=<frame,...> writes all 512 MB
// of guest physical memory at those presents. The game stalls while it writes.
void DumpMemoryAtFrame(rex::memory::Memory* memory, uint32_t frame) {
  static const std::vector<uint32_t> frames = [] {
    std::vector<uint32_t> out;
    std::string list = REXCVAR_GET(dump_memory_at);
    for (size_t pos = 0; pos < list.size();) {
      size_t end = list.find(',', pos);
      if (end == std::string::npos) end = list.size();
      out.push_back(uint32_t(std::strtoul(list.substr(pos, end - pos).c_str(), nullptr, 10)));
      pos = end + 1;
    }
    return out;
  }();
  if (std::find(frames.begin(), frames.end(), frame) == frames.end()) return;
  const std::string path = REXCVAR_GET(dump_memory) + "-" + std::to_string(frame) + ".bin";
  FILE* f = std::fopen(path.c_str(), "wb");
  if (!f) {
    REXLOG_ERROR("dump_memory: cannot write {}", path);
    return;
  }
  size_t written = std::fwrite(memory->TranslatePhysical<const uint8_t*>(0), 1, 0x20000000, f);
  std::fclose(f);
  REXLOG_INFO("dump_memory: frame {}: {} bytes -> {}", frame, written, path);
}

}  // namespace blitz
