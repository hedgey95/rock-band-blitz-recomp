// Native graphics system: replaces the SDK's emulated GPU plugin.
//
// Phase N1: no rendering yet. It keeps the game's GPU synchronisation alive
// (command ring consumption, fences, interrupts, vblank) with a sync-only walk
// of the command stream, and presents through the SDK's D3D12 presenter.
// See docs/native-renderer-plan.md.

#pragma once

#include <atomic>
#include <cstdint>
#include <memory>
#include <vector>

#include <rex/system/interfaces/graphics.h>
#include <rex/system/xobject.h>

#include "gamma_ramp.h"

namespace rex::memory {
class Memory;
class RingBuffer;
}  // namespace rex::memory
namespace rex::system {
class XHostThread;
}
namespace rex::thread {
class Event;
}

namespace blitz {

using rex::X_STATUS;

class TextureCache;
class ResourceListener;
class NativeRenderer;

// The native graphics system when --native_renderer is set, else null.
std::unique_ptr<rex::system::IGraphicsSystem> CreateNativeGraphicsIfRequested();

class NativeGraphicsSystem final : public rex::system::IGraphicsSystem {
 public:
  NativeGraphicsSystem();
  ~NativeGraphicsSystem() override;

  X_STATUS SetupPresentation(rex::ui::WindowedAppContext* app_context) override;
  X_STATUS SetupGuestGpu(rex::runtime::FunctionDispatcher* function_dispatcher,
                         rex::system::KernelState* kernel_state) override;
  bool has_presentation() const override { return presenter_ != nullptr; }
  rex::ui::GraphicsProvider* provider() const override;
  rex::ui::Presenter* presenter() const override { return presenter_.get(); }

  void SetInterruptCallback(uint32_t callback, uint32_t user_data) override;
  void InitializeRingBuffer(uint32_t ptr, uint32_t size_log2) override;
  void EnableReadPointerWriteBack(uint32_t ptr, uint32_t block_size_log2) override;
  void Shutdown() override;

  // Frames the game has swapped (PM4_XE_SWAP from VdSwap).
  uint64_t swap_count() const { return swap_count_.load(); }

 private:
  static uint32_t ReadRegisterThunk(void* ppc_context, void* self, uint32_t addr);
  static void WriteRegisterThunk(void* ppc_context, void* self, uint32_t addr, uint32_t value);
  uint32_t ReadRegister(uint32_t index);
  void WriteRegister(uint32_t index, uint32_t value);

  void RingThreadMain();
  void VblankThreadMain();
  void DispatchInterrupt(uint32_t source, uint32_t cpu);

  // Sync-only command walk: applies memory/register/interrupt side effects and
  // skips everything else (draws, state, constants).
  bool WalkPacket(rex::memory::RingBuffer& reader);
  bool WalkPacketType3(rex::memory::RingBuffer& reader, uint32_t packet);
  void WalkIndirectBuffer(uint32_t gpu_address, uint32_t dwords);
  uint32_t ReadPolled(uint32_t wait_info, uint32_t address);

  std::unique_ptr<rex::ui::GraphicsProvider> provider_;
  std::unique_ptr<rex::ui::Presenter> presenter_;

  // Native resources (phase N4): host textures for what D3D binds.
  std::unique_ptr<TextureCache> texture_cache_;
  std::unique_ptr<ResourceListener> resource_listener_;
  std::unique_ptr<NativeRenderer> renderer_;  // phase N5: native draws
  GammaRamp gamma_;  // the display gamma ramp, from the DC_LUT registers
  rex::ui::WindowedAppContext* app_context_ = nullptr;

  rex::memory::Memory* memory_ = nullptr;
  rex::runtime::FunctionDispatcher* function_dispatcher_ = nullptr;
  rex::system::KernelState* kernel_state_ = nullptr;

  std::vector<uint32_t> registers_;  // 0x10000 / 4 GPU registers

  uint32_t ring_ptr_ = 0;
  uint32_t ring_size_bytes_ = 0;
  uint32_t read_ptr_writeback_ = 0;
  uint32_t read_index_ = 0;
  std::atomic<uint32_t> write_index_{0};
  std::unique_ptr<rex::thread::Event> write_event_;

  uint64_t bin_mask_ = ~0ull;
  uint64_t bin_select_ = ~0ull;

  std::atomic<uint32_t> interrupt_callback_{0};
  std::atomic<uint32_t> interrupt_user_data_{0};
  std::atomic<uint32_t> counter_{0};  // vblanks + swaps, as the SDK's CP counter
  std::atomic<uint64_t> swap_count_{0};

  std::atomic<bool> running_{false};
  rex::system::object_ref<rex::system::XHostThread> ring_thread_;
  rex::system::object_ref<rex::system::XHostThread> vblank_thread_;
};

}  // namespace blitz
