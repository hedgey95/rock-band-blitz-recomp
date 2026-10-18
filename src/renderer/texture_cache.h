// Native renderer phase N4: host D3D12 textures for the game's texture fetch
// constants, created when D3D binds them.
#pragma once

#include <cstdint>
#include <map>
#include <memory>
#include <mutex>
#include <tuple>
#include <vector>

#include <rex/ui/d3d12/d3d12_api.h>

namespace rex::ui::d3d12 {
class D3D12Provider;
}

namespace blitz {

struct HostTexture;

class TextureCache {
 public:
  struct Entry {
    ID3D12Resource* resource = nullptr;
    DXGI_FORMAT format = DXGI_FORMAT_UNKNOWN;
    uint32_t mip_levels = 1, array_size = 1;
    bool cube = false;
    // SRV component mapping from the fetch constant swizzle.
    UINT component_mapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
    uint32_t width = 0, height = 0;
    // Written by GPU copies (resolves) rather than decoded from guest memory.
    bool resolve_target = false;
    D3D12_RESOURCE_STATES state =
        D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE | D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE;
  };

  explicit TextureCache(const rex::ui::d3d12::D3D12Provider& provider);
  ~TextureCache();

  // The texture for a fetch constant (host-order dwords as the GPU sees them),
  // decoded and uploaded on first use; null if its format is not supported.
  // `physical` points at guest physical address 0.
  Entry* Get(const uint32_t fetch[6], const uint8_t* physical);

  // The texture a resolve writes into (a GPU texture of the fetch constant's
  // size); it replaces any decoded texture at that address.
  // `swap_rb`: the copy swapped red and blue (RB_COPY_DEST_INFO copy_dest_swap);
  // the view swaps them over the unswapped host copy.
  Entry* ResolveTarget(const uint32_t fetch[6], DXGI_FORMAT format, bool swap_rb = false);

  // Looks up without creating.
  Entry* Find(const uint32_t fetch[6]);

  // With --check_texture_upload, every upload is read back from the GPU and
  // compared with the decoded data; these count the results.
  uint64_t checked_subresources() const { return checked_; }
  uint64_t mismatched_subresources() const { return mismatched_; }

 private:
  using Key = std::tuple<uint32_t, uint32_t, uint32_t, uint32_t>;
  static Key KeyOf(const uint32_t fetch[6]);

  bool Upload(const HostTexture& texture, Entry& entry);
  bool Execute(ID3D12GraphicsCommandList* list);

  const rex::ui::d3d12::D3D12Provider& provider_;
  ID3D12Device* device_ = nullptr;
  ID3D12CommandQueue* queue_ = nullptr;
  ID3D12CommandAllocator* allocator_ = nullptr;
  ID3D12GraphicsCommandList* list_ = nullptr;
  ID3D12Fence* fence_ = nullptr;
  HANDLE fence_event_ = nullptr;
  uint64_t fence_value_ = 0;

  std::mutex mutex_;
  std::map<Key, Entry> entries_;
  std::vector<ID3D12Resource*> retired_;
  bool check_ = false;
  uint64_t checked_ = 0, mismatched_ = 0, unsupported_ = 0;
};

}  // namespace blitz
