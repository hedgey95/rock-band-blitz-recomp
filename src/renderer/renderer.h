// Native renderer phase N5: draws the game's D3D calls on D3D12.
//
// A d3d::Listener (d3d_hooks.h) on the game's render thread:
// - the dirty-state flushes are replayed into a register file, which is
//   exactly what the GPU would see (docs/d3d-device-map.md);
// - shaders come from XenosRecomp, as DXIL files named by container
//   (--native_shader_dir, from tools/translate_shaders.py); one missing there is
//   translated on a worker thread at its first bind and saved there;
// - every draw call becomes a D3D12 draw with a pipeline built from the
//   registers, vertex/index data rebuilt for the host, and textures from the
//   texture cache;
// - clears, resolves and the present are D3D12 clears, copies and a compute
//   copy into the presenter's guest output.
// Frames are recorded into one command list and submitted at the present,
// which waits for the GPU (simple and correct first; milestone 4 measures).
#pragma once

#include <chrono>
#include <cstdint>
#include <functional>
#include <future>
#include <map>
#include <memory>
#include <string>
#include <unordered_map>
#include <vector>

#include <rex/ui/d3d12/d3d12_api.h>

#include "d3d_hooks.h"
#include "d3d_ops.h"
#include "gamma_ramp.h"
#include "texture_cache.h"
#include "vertex_input.h"

namespace rex::ui {
class Presenter;
}
namespace rex::ui::d3d12 {
class D3D12Provider;
}

namespace blitz {

class NativeRenderer final : public d3d::Listener {
 public:
  NativeRenderer(const rex::ui::d3d12::D3D12Provider& provider, rex::ui::Presenter* presenter,
                 const uint8_t* physical, TextureCache* textures);
  ~NativeRenderer() override;
  bool Initialize();
  // Where the present pass gets the display gamma ramp (the MMIO registers).
  void SetGammaSource(std::function<void(GammaRamp::Constants&)> source) { gamma_source_ = std::move(source); }

  void Before(uint32_t function, PPCContext& ctx, uint8_t* base) override;
  void After(uint32_t function, PPCContext& ctx, uint8_t* base) override;

 private:
  struct Shader {
    std::string name;
    bool vertex = false;
    std::vector<uint8_t> dxil;
    std::vector<VertexElement> elements;
    bool loaded = false;
    std::future<std::vector<uint8_t>> translating;  // DXIL, empty if translation failed
  };
  struct RenderTarget {
    ID3D12Resource* resource = nullptr;
    DXGI_FORMAT format = DXGI_FORMAT_UNKNOWN;
    uint32_t width = 0, height = 0;
    D3D12_CPU_DESCRIPTOR_HANDLE view{};
    D3D12_RESOURCE_STATES state = D3D12_RESOURCE_STATE_COMMON;
    bool depth = false;
    uint32_t srv_slot = 0;  // depth plane view for depth resolves
    uint32_t samples = 1;   // MSAA (RB_SURFACE_INFO)
  };
  struct Draw {
    uint32_t prim;
    bool indexed;
    uint32_t start, count;          // vertices, or indices for indexed draws
    uint32_t index_address;         // physical
    bool index32;
    uint32_t index_endian;
    int32_t base_vertex;
    // Inline vertices (Begin*Vertices): stream data in place of fetch slot 95.
    uint32_t inline_vertices;       // physical, 0 if none
    uint32_t inline_stride;
    uint32_t inline_size;           // bytes
  };

  // Frame
  bool BeginFrame();
  void EndFrame(uint32_t frontbuffer_texture, uint8_t* base);
  void Submit();
  D3D12_GPU_VIRTUAL_ADDRESS Upload(const void* data, size_t size, size_t alignment, void** mapped = nullptr);
  void Transition(ID3D12Resource* resource, D3D12_RESOURCE_STATES& state, D3D12_RESOURCE_STATES to);

  // State
  void ApplyFlush(uint32_t function, const PPCContext& ctx, uint8_t* base);
  Shader* ShaderFor(uint32_t object, uint8_t* base);
  void ReadBoundVertexShader(uint32_t from, uint32_t to, uint8_t* base);
  // The first value a call's packets wrote to `reg` with a type-0 packet, if any.
  bool FirstRegisterWrite(const std::vector<std::pair<uint32_t, uint32_t>>& ranges, uint32_t reg, uint8_t* base,
                          uint32_t* value);
  RenderTarget* ColorTarget();
  RenderTarget* DepthTarget();
  RenderTarget* Target(bool depth, uint32_t edram_base, uint32_t format);

  // Operations
  void DoDraw(const Draw& draw, uint8_t* base);
  void DoClear(const d3d::ClearOp& op, bool screen_rect = false);
  void DoResolve(const d3d::ResolveOp& op);
  void ResolveDepth(RenderTarget& source, TextureCache::Entry& dest, const D3D12_BOX& box, const int32_t point[2]);
  // --dump_resolves (dev): resolve destinations read back in captured frames.
  struct ResolveDump {
    uint32_t frame, fetch[6];
    ID3D12Resource* buffer;
    D3D12_PLACED_SUBRESOURCE_FOOTPRINT footprint;
    uint64_t size;
  };
  std::vector<ResolveDump> resolve_dumps_;
  bool DumpingResolves();
  void ReadBackResolve(TextureCache::Entry& dest, const uint32_t fetch[6]);
  void WriteResolveDumps();
  void BeginTiling(const PPCContext& ctx, uint8_t* base);

  // Draw helpers (renderer_draw.cpp)
  ID3D12PipelineState* Pipeline(const Shader& vs, const Shader& ps, uint32_t prim, RenderTarget* color,
                                RenderTarget* depth);
  // A guest vertex stream uploaded raw for this frame (once per frame per range).
  D3D12_GPU_VIRTUAL_ADDRESS StreamAddress(uint32_t address, uint32_t size);
  uint32_t TextureDescriptor(uint32_t slot, bool* cube);
  uint32_t SamplerDescriptor(uint32_t slot);
  void FillSharedConstants(uint8_t* shared);

  std::function<void(GammaRamp::Constants&)> gamma_source_;
  const rex::ui::d3d12::D3D12Provider& provider_;
  rex::ui::Presenter* presenter_;
  const uint8_t* physical_;
  TextureCache* textures_;
  ID3D12Device* device_ = nullptr;
  ID3D12CommandQueue* queue_ = nullptr;

  ID3D12RootSignature* root_signature_ = nullptr;
  ID3D12RootSignature* present_root_signature_ = nullptr;
  ID3D12PipelineState* present_pipeline_ = nullptr;
  ID3D12RootSignature* depth_copy_root_signature_ = nullptr;
  ID3D12PipelineState* depth_copy_pipeline_ = nullptr;
  ID3D12PipelineState* depth_copy_ms_pipeline_ = nullptr;
  std::unordered_map<ID3D12Resource*, uint32_t> uav_slots_;  // depth resolve destinations
  ID3D12DescriptorHeap* view_heap_ = nullptr;     // shader visible: [0, 1] present, [2, 4] null, then textures
  ID3D12DescriptorHeap* sampler_heap_ = nullptr;  // shader visible
  ID3D12DescriptorHeap* rtv_heap_ = nullptr;
  ID3D12DescriptorHeap* dsv_heap_ = nullptr;
  uint32_t view_increment_ = 0, sampler_increment_ = 0, rtv_increment_ = 0, dsv_increment_ = 0;
  // Null 2D, 3D and cube views for empty texture slots; sampler 0 is a default.
  static constexpr uint32_t kNullTexture2D = 2, kNullTexture3D = 3, kNullTextureCube = 4;
  uint32_t view_next_ = 5, sampler_next_ = 1, rtv_next_ = 0, dsv_next_ = 0;

  ID3D12CommandAllocator* allocator_ = nullptr;
  ID3D12GraphicsCommandList* list_ = nullptr;
  ID3D12GraphicsCommandList1* list1_ = nullptr;
  ID3D12Fence* fence_ = nullptr;
  HANDLE fence_event_ = nullptr;
  uint64_t fence_value_ = 0;
  bool recording_ = false, device_lost_ = false;
  std::vector<ID3D12Resource*> upload_buffers_;
  uint8_t* upload_mapped_ = nullptr;
  size_t upload_offset_ = 0, upload_size_ = 0;
  std::vector<ID3D12Resource*> frame_garbage_;

  // Register file the GPU would hold: [0x2000, 0x2400) and [0x4000, 0x4928).
  std::vector<uint32_t> regs_;
  uint32_t guest_device_ = 0;  // the game's D3D device

  std::unordered_map<uint32_t, std::pair<uint32_t, uint32_t>> registered_;  // object -> parts
  std::unordered_map<uint32_t, Shader*> shader_objects_;
  std::map<std::string, std::unique_ptr<Shader>> shaders_;
  uint32_t bound_vs_ = 0, bound_ps_ = 0;          // objects
  // The vertex shader microcode the GPU runs, copied at its IM_LOAD. D3D
  // re-patches the fetches in place when the vertex declaration changes, so
  // layouts are cached by content, not by address.
  std::vector<uint32_t> vs_code_;
  uint64_t vs_code_hash_ = 0;
  std::map<std::pair<std::string, uint64_t>, VertexFetches> fetches_;
  std::unordered_map<uint64_t, D3D12_GPU_VIRTUAL_ADDRESS> frame_streams_;

  std::map<uint64_t, RenderTarget> targets_;
  std::map<uint64_t, ID3D12PipelineState*> pipelines_;
  std::unordered_map<ID3D12Resource*, std::pair<uint32_t, uint32_t>> srv_indices_;  // index, mapping
  std::map<std::vector<uint32_t>, uint32_t> sampler_indices_;

  // Calls in flight on this thread (arguments kept for After).
  struct Pending {
    uint32_t function;
    // Shader flushes and resolves: the command buffer ranges the call wrote,
    // (from, to] each; a kick-off midway splits them across segments.
    uint32_t range_from;
    std::vector<std::pair<uint32_t, uint32_t>> ranges;
    Draw draw;
    d3d::ResolveOp resolve;
    d3d::ClearOp clear;
    uint32_t out_ptr;         // CreateShader: the container; BeginIndexedVertices: &indices
    uint32_t vertex_out_ptr;  // BeginIndexedVertices: &vertices
  };
  std::vector<Pending> pending_;
  Draw inline_draw_{};
  bool inline_pending_ = false;

  // Predicated tiling: the screen the tiles from BeginTiling cover. D3D replays
  // each call per tile; we see it once and draw the whole screen, so while the
  // surface is narrower than this, targets take this size and clears cover it.
  uint32_t tiled_width_ = 0, tiled_height_ = 0;
  bool Tiled() const { return (regs_[0x2000] & 0x3FFF) < tiled_width_; }

  // Statistics, logged once a second.
  uint64_t frames_ = 0, draws_ = 0, skipped_ = 0;
  uint64_t upload_bytes_ = 0, frame_upload_bytes_ = 0, max_frame_upload_bytes_ = 0;
  std::map<std::string, uint64_t> skip_reasons_;
  // Present-to-present times over the same 600 frames (milestone 4).
  std::chrono::steady_clock::time_point last_present_{};
  double frame_ms_sum_ = 0, frame_ms_max_ = 0;
  uint32_t slow_frames_ = 0;  // over 1.5 vblanks (25 ms)
  void Skip(const std::string& reason);
};

}  // namespace blitz
