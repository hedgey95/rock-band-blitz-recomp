// Native graphics system, phase N1: GPU sync without GPU emulation.
//
// The game's XDK D3D still writes PM4 packets into its command ring. Instead of
// emulating a GPU we walk the stream for the side effects the CPU waits on:
// fence values written to memory (MEM_WRITE, EVENT_WRITE_SHD, scratch
// registers, COND_WRITE, REG_TO_MEM), interrupts, occlusion-query results, and
// the swap marker VdSwap inserts. Draws, state and constants are skipped. The
// packet semantics mirror the SDK's CommandProcessor (Xenia).

#include "native_graphics.h"

#include "d3d_hooks.h"
#include "renderer.h"
#include "texture_cache.h"

#include <algorithm>
#include <chrono>
#include <thread>

#include <rex/chrono/clock.h>
#include <rex/cvar.h>
#include <rex/graphics/xenos.h>
#include <rex/kernel/xboxkrnl/video.h>
#include <rex/logging.h>
#include <rex/memory/ring_buffer.h>
#include <rex/memory/utils.h>
#include <rex/system/function_dispatcher.h>
#include <rex/system/kernel_state.h>
#include <rex/system/xmemory.h>
#include <rex/system/xthread.h>
#include <rex/thread.h>
#include <rex/ui/d3d12/d3d12_provider.h>
#include <rex/ui/presenter.h>
#include <rex/ui/windowed_app_context.h>

REXCVAR_DEFINE_BOOL(native_renderer, false, "Blitz",
                    "Use the native renderer instead of --gpu_plugin (in development)");

namespace blitz {

namespace {

namespace xenos = rex::graphics::xenos;

// Register indices (dword offsets into the 0x7FC80000 MMIO block); names as in
// the SDK's include/rex/graphics/register_table.inc.
constexpr uint32_t kRegCount = 0x10000 / 4;
constexpr uint32_t kCpRbWptr = 0x01C5;
constexpr uint32_t kScratchUmsk = 0x01DC;
constexpr uint32_t kScratchAddr = 0x01DD;
constexpr uint32_t kScratchReg0 = 0x0578;
constexpr uint32_t kScratchReg7 = 0x057F;
constexpr uint32_t kCoherStatusHost = 0x0A31;
constexpr uint32_t kVgtEventInitiator = 0x21F9;
constexpr uint32_t kRbSampleCountAddr = 0x2325;

uint32_t Physical(uint32_t gpu_address) { return gpu_address & 0x1FFFFFFF; }

}  // namespace

// Feeds what D3D binds to the native resources: each texture fetch constant
// D3D_FlushFetchConstants sends gets a host texture.
class ResourceListener final : public d3d::Listener {
 public:
  ResourceListener(TextureCache* textures, const uint8_t* physical)
      : textures_(textures), physical_(physical) {}

  void Before(uint32_t function, PPCContext& ctx, uint8_t* base) override {
    if (function == d3d::kFlushFetchConstants) {
      t_device = ctx.r3.u32;
      t_mask = ctx.r4.u64;
    }
  }

  void After(uint32_t function, PPCContext& ctx, uint8_t* base) override {
    if (function != d3d::kFlushFetchConstants) return;
    for (uint32_t slot = 0; slot < 32; ++slot) {
      if (!((t_mask >> (63 - slot)) & 1)) continue;  // bit 63 is slot 0
      uint32_t fetch[6];
      for (int i = 0; i < 6; ++i)
        fetch[i] = rex::memory::load_and_swap<uint32_t>(
            base + t_device + d3d::kDeviceFetchConstants + 24 * slot + 4 * i);
      if ((fetch[0] & 3) == 2) textures_->Get(fetch, physical_);
    }
  }

 private:
  TextureCache* textures_;
  const uint8_t* physical_;
  static thread_local uint32_t t_device;
  static thread_local uint64_t t_mask;
};

thread_local uint32_t ResourceListener::t_device = 0;
thread_local uint64_t ResourceListener::t_mask = 0;

std::unique_ptr<rex::system::IGraphicsSystem> CreateNativeGraphicsIfRequested() {
  if (!REXCVAR_GET(native_renderer)) return nullptr;
  return std::make_unique<NativeGraphicsSystem>();
}

NativeGraphicsSystem::NativeGraphicsSystem()
    : registers_(kRegCount, 0), write_event_(rex::thread::Event::CreateAutoResetEvent(false)) {}

NativeGraphicsSystem::~NativeGraphicsSystem() { Shutdown(); }

rex::ui::GraphicsProvider* NativeGraphicsSystem::provider() const { return provider_.get(); }

X_STATUS NativeGraphicsSystem::SetupPresentation(rex::ui::WindowedAppContext* app_context) {
  if (presenter_) return X_STATUS_SUCCESS;
  if (!provider_) provider_ = rex::ui::d3d12::D3D12Provider::Create();
  if (!provider_) {
    REXLOG_ERROR("native_renderer: no D3D12 device");
    return X_STATUS_UNSUCCESSFUL;
  }
  app_context_ = app_context;
  auto on_loss = [](bool is_responsible, bool) {
    REXLOG_ERROR("native_renderer: host GPU lost (responsible={})", is_responsible);
  };
  if (app_context_) {
    app_context_->CallInUIThreadSynchronous(
        [this, on_loss]() { presenter_ = provider_->CreatePresenter(on_loss); });
  } else {
    presenter_ = provider_->CreatePresenter(on_loss);
  }
  return presenter_ ? X_STATUS_SUCCESS : X_STATUS_UNSUCCESSFUL;
}

X_STATUS NativeGraphicsSystem::SetupGuestGpu(rex::runtime::FunctionDispatcher* function_dispatcher,
                                             rex::system::KernelState* kernel_state) {
  function_dispatcher_ = function_dispatcher;
  memory_ = function_dispatcher->memory();
  kernel_state_ = kernel_state;
  if (!provider_) provider_ = rex::ui::d3d12::D3D12Provider::Create();

  memory_->AddVirtualMappedRange(0x7FC80000, 0xFFFF0000, 0x0000FFFF, this, ReadRegisterThunk,
                                 WriteRegisterThunk);

  running_ = true;
  ring_thread_ = rex::system::object_ref<rex::system::XHostThread>(
      new rex::system::XHostThread(kernel_state_, 128 * 1024, 0, [this]() {
        RingThreadMain();
        return 0;
      }));
  ring_thread_->set_name("Native GPU sync");
  ring_thread_->Create();
  vblank_thread_ = rex::system::object_ref<rex::system::XHostThread>(
      new rex::system::XHostThread(kernel_state_, 128 * 1024, 0, [this]() {
        VblankThreadMain();
        return 0;
      }));
  vblank_thread_->set_name("Native GPU vblank");
  vblank_thread_->Create();
  // Listeners must be added before the game runs; it has not started yet.
  texture_cache_ = std::make_unique<TextureCache>(
      static_cast<const rex::ui::d3d12::D3D12Provider&>(*provider_));
  resource_listener_ = std::make_unique<ResourceListener>(
      texture_cache_.get(), memory_->TranslatePhysical<const uint8_t*>(0));
  d3d::AddListener(resource_listener_.get());
  renderer_ = std::make_unique<NativeRenderer>(
      static_cast<const rex::ui::d3d12::D3D12Provider&>(*provider_), presenter_.get(),
      memory_->TranslatePhysical<const uint8_t*>(0), texture_cache_.get());
  renderer_->SetGammaSource([this](GammaRamp::Constants& out) { gamma_.Get(registers_.data(), out); });
  if (renderer_->Initialize()) {
    d3d::AddListener(renderer_.get());
  } else {
    REXLOG_ERROR("native_renderer: D3D12 renderer setup failed; nothing will be drawn");
  }
  REXLOG_INFO("native_renderer: running without GPU emulation (sync, native draws)");
  return X_STATUS_SUCCESS;
}

void NativeGraphicsSystem::Shutdown() {
  if (!running_.exchange(false)) return;
  write_event_->Set();
  if (ring_thread_) ring_thread_->Wait(0, 0, 0, nullptr);
  if (vblank_thread_) vblank_thread_->Wait(0, 0, 0, nullptr);
  ring_thread_.reset();
  vblank_thread_.reset();
  presenter_.reset();
  provider_.reset();
}

void NativeGraphicsSystem::SetInterruptCallback(uint32_t callback, uint32_t user_data) {
  interrupt_user_data_ = user_data;
  interrupt_callback_ = callback;
}

void NativeGraphicsSystem::InitializeRingBuffer(uint32_t ptr, uint32_t size_log2) {
  read_index_ = 0;
  ring_ptr_ = ptr;
  ring_size_bytes_ = uint32_t(1) << (size_log2 + 3);
}

void NativeGraphicsSystem::EnableReadPointerWriteBack(uint32_t ptr, uint32_t) {
  read_ptr_writeback_ = ptr;
}

uint32_t NativeGraphicsSystem::ReadRegisterThunk(void*, void* self, uint32_t addr) {
  return static_cast<NativeGraphicsSystem*>(self)->ReadRegister((addr & 0xFFFF) / 4);
}

void NativeGraphicsSystem::WriteRegisterThunk(void*, void* self, uint32_t addr, uint32_t value) {
  static_cast<NativeGraphicsSystem*>(self)->WriteRegister((addr & 0xFFFF) / 4, value);
}

uint32_t NativeGraphicsSystem::ReadRegister(uint32_t index) {
  rex::system::X_VIDEO_MODE mode;
  switch (index) {
    case 0x0F00:  // RB_EDRAM_TIMING
      return 0x08100748;
    case 0x0F01:  // RB_BC_CONTROL
      return 0x0000200E;
    case 0x194C:  // D1MODE_V_COUNTER
      rex::kernel::xboxkrnl::VdQueryVideoMode(&mode);
      return std::min(uint32_t(mode.display_height), 0x0FFFu);
    case 0x1951:  // interrupt status: vblank
      return 1;
    case 0x1961:  // D1MODE_VIEWPORT_SIZE
      rex::kernel::xboxkrnl::VdQueryVideoMode(&mode);
      return (std::min(uint32_t(mode.display_width), 0x0FFFu) << 16) |
             std::min(uint32_t(mode.display_height), 0x0FFFu);
    default:
      return index < kRegCount ? registers_[index] : 0;
  }
}

void NativeGraphicsSystem::WriteRegister(uint32_t index, uint32_t value) {
  if (index >= kRegCount) return;
  const_cast<volatile uint32_t&>(registers_[index]) = value;
  if (index == kCpRbWptr) {
    write_index_ = value;
    write_event_->Set();
  } else if (index >= kScratchReg0 && index <= kScratchReg7) {
    uint32_t reg = index - kScratchReg0;
    if (registers_[kScratchUmsk] & (1u << reg)) {
      rex::memory::store_and_swap<uint32_t>(
          memory_->TranslatePhysical(registers_[kScratchAddr] + reg * 4), value);
    }
  } else if (index == kCoherStatusHost) {
    // Memory is always coherent here; clear the pending bit right away.
    const_cast<volatile uint32_t&>(registers_[index]) = value & 0x7FFFFFFF;
  } else if (GammaRamp::Watches(index)) {
    gamma_.Write(index, value, registers_.data());
  }
}

void NativeGraphicsSystem::DispatchInterrupt(uint32_t source, uint32_t cpu) {
  uint32_t callback = interrupt_callback_;
  if (!callback) return;
  auto* thread = rex::system::XThread::GetCurrentThread();
  thread->SetActiveCpu(uint8_t(cpu));
  uint64_t args[] = {source, interrupt_user_data_};
  function_dispatcher_->ExecuteInterrupt(thread->thread_state(), callback, args, 2);
}

void NativeGraphicsSystem::VblankThreadMain() {
  rex::system::X_VIDEO_MODE mode;
  rex::kernel::xboxkrnl::VdQueryVideoMode(&mode);
  const double hz = std::max(1.0, double(float(mode.refresh_rate)));
  const uint64_t interval =
      std::max<uint64_t>(1, uint64_t(double(rex::chrono::Clock::guest_tick_frequency()) / hz));
  uint64_t last = rex::chrono::Clock::QueryPacingTickCount();
  uint32_t vblanks = 0;
  uint64_t beat_swaps = 0;
  auto beat_time = std::chrono::steady_clock::now();
  while (running_) {
    uint64_t now = rex::chrono::Clock::QueryPacingTickCount();
    while (now - last >= interval) {
      counter_++;
      DispatchInterrupt(0, 2);
      last += interval;
      if (++vblanks % 600 == 0) {  // heartbeat: swaps the game issued over ~10 s
        auto t = std::chrono::steady_clock::now();
        uint64_t swaps = swap_count_.load();
        REXLOG_INFO("native_renderer: {} swaps, {:.2f} fps", swaps,
                    (swaps - beat_swaps) / std::chrono::duration<double>(t - beat_time).count());
        beat_swaps = swaps;
        beat_time = t;
      }
    }
    rex::thread::Sleep(std::chrono::milliseconds(1));
  }
}

void NativeGraphicsSystem::RingThreadMain() {
  while (running_) {
    uint32_t write_index = write_index_.load();
    if (!ring_size_bytes_ || write_index == read_index_) {
      rex::thread::Wait(write_event_.get(), true, std::chrono::milliseconds(50));
      continue;
    }
    rex::memory::RingBuffer reader(memory_->TranslatePhysical<uint8_t*>(ring_ptr_),
                                   ring_size_bytes_);
    reader.set_read_offset(read_index_ * 4);
    reader.set_write_offset(write_index * 4);
    while (reader.read_count() && running_) {
      if (!WalkPacket(reader)) {
        REXLOG_ERROR("native_renderer: bad packet in the primary ring");
        break;
      }
    }
    read_index_ = write_index;
    if (read_ptr_writeback_) {
      rex::memory::store_and_swap<uint32_t>(memory_->TranslatePhysical(read_ptr_writeback_),
                                            read_index_);
    }
  }
}

void NativeGraphicsSystem::WalkIndirectBuffer(uint32_t gpu_address, uint32_t dwords) {
  rex::memory::RingBuffer reader(memory_->TranslatePhysical<uint8_t*>(Physical(gpu_address)),
                                 dwords * 4);
  reader.set_write_offset(dwords * 4);
  do {
    if (!WalkPacket(reader)) {
      REXLOG_ERROR("native_renderer: bad packet in an indirect buffer");
      break;
    }
  } while (reader.read_count());
}

bool NativeGraphicsSystem::WalkPacket(rex::memory::RingBuffer& reader) {
  const uint32_t packet = reader.ReadAndSwap<uint32_t>();
  if (packet == 0) return true;
  switch (packet >> 30) {
    case 0: {  // register writes
      uint32_t count = ((packet >> 16) & 0x3FFF) + 1;
      if (reader.read_count() < count * 4) return false;
      uint32_t base = packet & 0x7FFF;
      bool one_reg = (packet >> 15) & 1;
      for (uint32_t i = 0; i < count; ++i)
        WriteRegister(one_reg ? base : base + i, reader.ReadAndSwap<uint32_t>());
      return true;
    }
    case 1: {  // two register writes
      uint32_t r1 = packet & 0x7FF, r2 = (packet >> 11) & 0x7FF;
      uint32_t v1 = reader.ReadAndSwap<uint32_t>(), v2 = reader.ReadAndSwap<uint32_t>();
      WriteRegister(r1, v1);
      WriteRegister(r2, v2);
      return true;
    }
    case 2:  // no-op
      return true;
    default:
      return WalkPacketType3(reader, packet);
  }
}

uint32_t NativeGraphicsSystem::ReadPolled(uint32_t wait_info, uint32_t address) {
  if (wait_info & 0x10) {  // memory
    uint32_t value = rex::memory::load<uint32_t>(memory_->TranslatePhysical(address & ~3u));
    return xenos::GpuSwap(value, xenos::Endian(address & 3));
  }
  if (address == kCoherStatusHost) registers_[address] &= 0x7FFFFFFF;
  return ReadRegister(address);
}

bool NativeGraphicsSystem::WalkPacketType3(rex::memory::RingBuffer& reader, uint32_t packet) {
  const uint32_t opcode = (packet >> 8) & 0x7F;
  const uint32_t count = ((packet >> 16) & 0x3FFF) + 1;
  if (reader.read_count() < count * 4) return false;
  uint32_t used = 0;
  auto next = [&]() {
    ++used;
    return reader.ReadAndSwap<uint32_t>();
  };
  auto store = [&](uint32_t address, uint32_t value) {
    value = xenos::GpuSwap(value, xenos::Endian(address & 3));
    rex::memory::store<uint32_t>(memory_->TranslatePhysical(address & ~3u), value);
  };
  auto matches = [](uint32_t func, uint32_t value, uint32_t ref) {
    switch (func & 7) {
      case 1: return value < ref;
      case 2: return value <= ref;
      case 3: return value == ref;
      case 4: return value != ref;
      case 5: return value >= ref;
      case 6: return value > ref;
      case 7: return true;
      default: return false;
    }
  };

  // Predicated packets run only for the bins selected by tiled rendering.
  if ((packet & 1) && ((bin_select_ & bin_mask_) == 0 || opcode == xenos::PM4_XE_SWAP)) {
    reader.AdvanceRead(count * 4);
    return true;
  }

  switch (opcode) {
    case xenos::PM4_INTERRUPT: {
      uint32_t cpu_mask = next();
      for (uint32_t n = 0; n < 6; ++n)
        if (cpu_mask & (1u << n)) DispatchInterrupt(1, n);
      break;
    }
    case xenos::PM4_XE_SWAP:
      swap_count_++;
      counter_++;
      break;
    case xenos::PM4_INDIRECT_BUFFER:
    case xenos::PM4_INDIRECT_BUFFER_PFD: {
      uint32_t ptr = next();
      uint32_t length = next() & 0xFFFFF;
      WalkIndirectBuffer(ptr, length);
      break;
    }
    case xenos::PM4_WAIT_REG_MEM: {
      uint32_t wait_info = next(), address = next(), ref = next(), mask = next(), wait = next();
      while (running_ && !matches(wait_info, ReadPolled(wait_info, address) & mask, ref)) {
        if (wait >= 0x100)
          rex::thread::Sleep(std::chrono::milliseconds(wait / 0x100));
        else
          std::this_thread::yield();
      }
      break;
    }
    case xenos::PM4_REG_RMW: {
      uint32_t info = next(), and_mask = next(), or_mask = next();
      uint32_t value = registers_[info & 0x1FFF];
      value &= (info >> 31) & 1 ? registers_[and_mask & 0x1FFF] : and_mask;
      value |= (info >> 30) & 1 ? registers_[or_mask & 0x1FFF] : or_mask;
      WriteRegister(info & 0x1FFF, value);
      break;
    }
    case xenos::PM4_REG_TO_MEM: {
      uint32_t reg = next(), address = next();
      store(address, ReadRegister(reg));
      break;
    }
    case xenos::PM4_MEM_WRITE: {
      uint32_t address = next();
      while (used < count) {
        store(address, next());
        address += 4;
      }
      break;
    }
    case xenos::PM4_COND_WRITE: {
      uint32_t wait_info = next(), poll = next(), ref = next(), mask = next();
      uint32_t target = next(), data = next();
      if (matches(wait_info, ReadPolled(wait_info, poll) & mask, ref)) {
        if (wait_info & 0x100)
          store(target, data);
        else
          WriteRegister(target, data);
      }
      break;
    }
    case xenos::PM4_EVENT_WRITE:
      WriteRegister(kVgtEventInitiator, next() & 0x3F);
      break;
    case xenos::PM4_EVENT_WRITE_SHD: {
      uint32_t initiator = next(), address = next(), value = next();
      WriteRegister(kVgtEventInitiator, initiator & 0x3F);
      store(address, (initiator >> 31) & 1 ? counter_.load() : value);
      break;
    }
    case xenos::PM4_EVENT_WRITE_EXT: {
      uint32_t initiator = next(), address = next();
      WriteRegister(kVgtEventInitiator, initiator & 0x3F);
      // Screen extents of the previous draw: report "everything".
      uint16_t extents[] = {0, xenos::kTexture2DCubeMaxWidthHeight >> 3, 0,
                            xenos::kTexture2DCubeMaxWidthHeight >> 3, 0, 1};
      rex::memory::copy_and_swap_16_unaligned(memory_->TranslatePhysical(address & ~3u), extents,
                                              6);
      break;
    }
    case xenos::PM4_EVENT_WRITE_ZPD: {
      WriteRegister(kVgtEventInitiator, next() & 0x3F);
      // Occlusion queries: report a fixed number of passed samples, like the
      // SDK's query_occlusion_fake_sample_count default.
      auto* counts = memory_->TranslatePhysical<xenos::xe_gpu_depth_sample_counts*>(
          registers_[kRbSampleCountAddr]);
      if (counts) {
        const uint32_t kQueryFinished = rex::byte_swap(0xFFFFFEEDu);
        bool ended = counts->ZPass_A == kQueryFinished || counts->ZPass_B == kQueryFinished ||
                     counts->ZFail_A == kQueryFinished || counts->ZFail_B == kQueryFinished;
        std::memset(counts, 0, sizeof(*counts));
        if (ended) {
          counts->ZPass_A = 1000;
          counts->Total_A = 1000;
        }
      }
      break;
    }
    case xenos::PM4_SET_BIN_MASK_LO:
      bin_mask_ = (bin_mask_ & 0xFFFFFFFF00000000ull) | next();
      break;
    case xenos::PM4_SET_BIN_MASK_HI:
      bin_mask_ = (bin_mask_ & 0xFFFFFFFFull) | (uint64_t(next()) << 32);
      break;
    case xenos::PM4_SET_BIN_SELECT_LO:
      bin_select_ = (bin_select_ & 0xFFFFFFFF00000000ull) | next();
      break;
    case xenos::PM4_SET_BIN_SELECT_HI:
      bin_select_ = (bin_select_ & 0xFFFFFFFFull) | (uint64_t(next()) << 32);
      break;
    case xenos::PM4_SET_BIN_MASK: {
      uint64_t hi = next(), lo = next();
      bin_mask_ = (hi << 32) | lo;
      break;
    }
    case xenos::PM4_SET_BIN_SELECT: {
      uint64_t hi = next(), lo = next();
      bin_select_ = (hi << 32) | lo;
      break;
    }
    default:  // draws, state, constants, shader loads: nothing to sync
      break;
  }
  if (used > count) return false;
  reader.AdvanceRead((count - used) * 4);
  return true;
}

}  // namespace blitz
