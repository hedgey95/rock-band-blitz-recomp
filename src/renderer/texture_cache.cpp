// Native renderer phase N4: host D3D12 textures for the game's texture fetch
// constants. See texture_cache.h.
//
// Uploads are synchronous for now (one command list, waited on): textures are
// created when first bound, a few hundred per run. Batching into the frame's
// command list comes with the draws (N5). Contents are keyed by the fetch
// constant's identity only; textures the game rewrites in place (the
// k_32_32_32_32 data textures) need change tracking, also N5.

#include "texture_cache.h"

#include <cstring>
#include <string>
#include <vector>

#include <rex/cvar.h>
#include <rex/logging.h>
#include <rex/ui/d3d12/d3d12_provider.h>
#include <rex/ui/d3d12/d3d12_util.h>

#include "texture_decode.h"

REXCVAR_DEFINE_BOOL(check_texture_upload, false, "Blitz",
                    "Dev: read every uploaded texture back from the GPU and compare it");

namespace blitz {

namespace {

namespace d3d12_util = rex::ui::d3d12::util;

// Fetch constant swizzle (3 bits per destination xyzw: 0-3 source RGBA, 4 = 0,
// 5 = 1) as a D3D12 SRV component mapping.
// The fetch constant's swizzle over the host texture. Formats with fewer than
// four channels repeat their last one (the SDK's host swizzles: RRRR, RGGG, RGBB),
// e.g. a depth texture reads as depth in every channel.
UINT ComponentMapping(uint32_t swizzle, uint32_t format, bool swap_rb = false) {
  uint32_t last = 3;
  switch (format) {
    case 0: case 1: case 2: case 8: case 9: case 22: case 23: case 24: case 30: case 33: case 36:
    case 39: case 41: case 58: case 59:
      last = 0;
      break;
    case 10: case 25: case 31: case 34: case 49:  // 8_8, 16_16, 16_16_FLOAT, 32_32, DXN
      last = 1;
      break;
    case 4: case 16: case 17: case 57:  // 5_6_5, 10_11_11, 11_11_10, 32_32_32_FLOAT
      last = 2;
      break;
    default:
      break;
  }
  auto component = [&](int i) -> UINT {
    uint32_t c = (swizzle >> (3 * i)) & 7;
    if (swap_rb && (c == 0 || c == 2)) c ^= 2;
    switch (c < 4 ? std::min(c, last) : c) {
      case 0: return D3D12_SHADER_COMPONENT_MAPPING_FROM_MEMORY_COMPONENT_0;
      case 1: return D3D12_SHADER_COMPONENT_MAPPING_FROM_MEMORY_COMPONENT_1;
      case 2: return D3D12_SHADER_COMPONENT_MAPPING_FROM_MEMORY_COMPONENT_2;
      case 3: return D3D12_SHADER_COMPONENT_MAPPING_FROM_MEMORY_COMPONENT_3;
      case 4: return D3D12_SHADER_COMPONENT_MAPPING_FORCE_VALUE_0;
      default: return D3D12_SHADER_COMPONENT_MAPPING_FORCE_VALUE_1;
    }
  };
  return D3D12_ENCODE_SHADER_4_COMPONENT_MAPPING(component(0), component(1), component(2),
                                                 component(3));
}

ID3D12Resource* CreateBuffer(ID3D12Device* device, const D3D12_HEAP_PROPERTIES& heap,
                             uint64_t size, D3D12_RESOURCE_STATES state) {
  D3D12_RESOURCE_DESC desc = {};
  desc.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
  desc.Width = size;
  desc.Height = desc.DepthOrArraySize = desc.MipLevels = 1;
  desc.SampleDesc.Count = 1;
  desc.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
  ID3D12Resource* buffer = nullptr;
  if (FAILED(device->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE, &desc, state, nullptr,
                                             IID_PPV_ARGS(&buffer))))
    return nullptr;
  return buffer;
}

}  // namespace

TextureCache::TextureCache(const rex::ui::d3d12::D3D12Provider& provider) : provider_(provider) {
  device_ = provider.GetDevice();
  queue_ = provider.GetDirectQueue();
  check_ = REXCVAR_GET(check_texture_upload);
  if (FAILED(device_->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT,
                                             IID_PPV_ARGS(&allocator_))) ||
      FAILED(device_->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, allocator_, nullptr,
                                        IID_PPV_ARGS(&list_))) ||
      FAILED(device_->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&fence_)))) {
    REXLOG_ERROR("texture_cache: cannot create the upload command list");
    return;
  }
  list_->Close();
  fence_event_ = CreateEvent(nullptr, FALSE, FALSE, nullptr);
}

TextureCache::~TextureCache() {
  for (auto& [key, entry] : entries_)
    if (entry.resource) entry.resource->Release();
  for (auto* resource : retired_) resource->Release();
  if (list_) list_->Release();
  if (allocator_) allocator_->Release();
  if (fence_) fence_->Release();
  if (fence_event_) CloseHandle(fence_event_);
}

TextureCache::Key TextureCache::KeyOf(const uint32_t fetch[6]) {
  // Tiling and pitch, format and base address, size, mip address.
  return {fetch[0] & 0xFFC00000u, fetch[1], fetch[2], fetch[5] & 0xFFFFF800u};
}

TextureCache::Entry* TextureCache::Find(const uint32_t fetch[6]) {
  std::lock_guard lock(mutex_);
  auto it = entries_.find(KeyOf(fetch));
  return it != entries_.end() && it->second.resource ? &it->second : nullptr;
}

TextureCache::Entry* TextureCache::ResolveTarget(const uint32_t fetch[6], DXGI_FORMAT format, bool swap_rb) {
  std::lock_guard lock(mutex_);
  Entry& entry = entries_[KeyOf(fetch)];
  const uint32_t width = (fetch[2] & 0x1FFF) + 1, height = ((fetch[2] >> 13) & 0x1FFF) + 1;
  if (entry.resource && entry.resolve_target && entry.format == format && entry.width == width &&
      entry.height == height) {
    entry.component_mapping = ComponentMapping((fetch[3] >> 1) & 0xFFF, fetch[1] & 0x3F, swap_rb);
    return &entry;
  }
  // shortcut: a replaced texture lives until exit, so the renderer's in-flight
  // draws and its SRVs (keyed by resource) stay valid; free by fence if this grows.
  if (entry.resource) retired_.push_back(entry.resource);
  entry = Entry{};
  D3D12_RESOURCE_DESC desc = {};
  desc.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
  desc.Width = width;
  desc.Height = height;
  desc.DepthOrArraySize = 1;
  desc.MipLevels = 1;
  desc.Format = format;
  desc.SampleDesc.Count = 1;
  // Depth resolves (R32_FLOAT) are written by a compute shader.
  if (format == DXGI_FORMAT_R32_FLOAT) desc.Flags = D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS;
  if (FAILED(device_->CreateCommittedResource(&d3d12_util::kHeapPropertiesDefault,
                                              D3D12_HEAP_FLAG_NONE, &desc, entry.state, nullptr,
                                              IID_PPV_ARGS(&entry.resource)))) {
    REXLOG_ERROR("texture_cache: cannot create a {}x{} resolve target", width, height);
    entry.resource = nullptr;
    return nullptr;
  }
  entry.format = format;
  entry.width = width;
  entry.height = height;
  entry.resolve_target = true;
  // Resolve targets are sampled with the fetch constant's swizzle too.
  entry.component_mapping = ComponentMapping((fetch[3] >> 1) & 0xFFF, fetch[1] & 0x3F, swap_rb);
  return &entry;
}

TextureCache::Entry* TextureCache::Get(const uint32_t fetch[6], const uint8_t* physical) {
  if (!list_) return nullptr;
  const Key key = KeyOf(fetch);
  std::lock_guard lock(mutex_);
  auto it = entries_.find(key);
  if (it != entries_.end()) return it->second.resource ? &it->second : nullptr;

  Entry& entry = entries_[key];
  HostTexture texture;
  std::string error;
  if (!DecodeTexture(fetch, physical, &texture, &error)) {
    ++unsupported_;
    REXLOG_WARN("texture_cache: {:08X}: {}", fetch[1] & 0xFFFFF000, error);
    return nullptr;
  }
  entry.format = texture.format;
  entry.mip_levels = texture.mip_levels;
  entry.array_size = texture.array_size;
  entry.cube = texture.cube;
  entry.width = texture.width;
  entry.height = texture.height;
  entry.component_mapping = ComponentMapping(texture.swizzle, fetch[1] & 0x3F);
  if (!Upload(texture, entry)) return nullptr;
  if (check_ && entries_.size() % 50 == 0)
    REXLOG_INFO("texture_cache: {} textures; upload check: {} subresources read back, {} differ",
                entries_.size(), checked_, mismatched_);
  return &entry;
}

bool TextureCache::Execute(ID3D12GraphicsCommandList* list) {
  if (FAILED(list->Close())) return false;
  ID3D12CommandList* lists[] = {list};
  queue_->ExecuteCommandLists(1, lists);
  queue_->Signal(fence_, ++fence_value_);
  if (fence_->GetCompletedValue() < fence_value_) {
    fence_->SetEventOnCompletion(fence_value_, fence_event_);
    WaitForSingleObject(fence_event_, INFINITE);
  }
  return true;
}

bool TextureCache::Upload(const HostTexture& texture, Entry& entry) {
  D3D12_RESOURCE_DESC desc = {};
  desc.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
  desc.Width = texture.width;
  desc.Height = texture.height;
  desc.DepthOrArraySize = UINT16(texture.array_size);
  desc.MipLevels = UINT16(texture.mip_levels);
  desc.Format = texture.format;
  desc.SampleDesc.Count = 1;
  if (FAILED(device_->CreateCommittedResource(&d3d12_util::kHeapPropertiesDefault,
                                              D3D12_HEAP_FLAG_NONE, &desc,
                                              D3D12_RESOURCE_STATE_COPY_DEST, nullptr,
                                              IID_PPV_ARGS(&entry.resource)))) {
    REXLOG_ERROR("texture_cache: cannot create a {}x{} texture (format {})", texture.width,
                 texture.height, uint32_t(texture.format));
    return false;
  }

  const UINT count = UINT(texture.subresources.size());
  std::vector<D3D12_PLACED_SUBRESOURCE_FOOTPRINT> layouts(count);
  std::vector<UINT> rows(count);
  std::vector<UINT64> row_sizes(count);
  UINT64 total = 0;
  device_->GetCopyableFootprints(&desc, 0, count, 0, layouts.data(), rows.data(),
                                 row_sizes.data(), &total);
  ID3D12Resource* upload =
      CreateBuffer(device_, d3d12_util::kHeapPropertiesUpload, total, D3D12_RESOURCE_STATE_GENERIC_READ);
  if (!upload) return false;
  uint8_t* mapped = nullptr;
  upload->Map(0, nullptr, reinterpret_cast<void**>(&mapped));
  for (UINT i = 0; i < count; ++i) {
    const auto& sub = texture.subresources[i];
    for (UINT row = 0; row < rows[i] && row < sub.rows; ++row)
      std::memcpy(mapped + layouts[i].Offset + size_t(row) * layouts[i].Footprint.RowPitch,
                  sub.data.data() + size_t(row) * sub.row_pitch,
                  std::min<size_t>(sub.row_pitch, size_t(row_sizes[i])));
  }
  upload->Unmap(0, nullptr);

  allocator_->Reset();
  list_->Reset(allocator_, nullptr);
  for (UINT i = 0; i < count; ++i) {
    D3D12_TEXTURE_COPY_LOCATION dst = {entry.resource, D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX};
    dst.SubresourceIndex = i;
    D3D12_TEXTURE_COPY_LOCATION src = {upload, D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT};
    src.PlacedFootprint = layouts[i];
    list_->CopyTextureRegion(&dst, 0, 0, 0, &src, nullptr);
  }
  ID3D12Resource* readback = nullptr;
  D3D12_RESOURCE_BARRIER barrier = {};
  barrier.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
  barrier.Transition.pResource = entry.resource;
  barrier.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
  barrier.Transition.StateBefore = D3D12_RESOURCE_STATE_COPY_DEST;
  if (check_) {
    // Copy it straight back out to compare with what was uploaded.
    readback = CreateBuffer(device_, d3d12_util::kHeapPropertiesReadback, total,
                            D3D12_RESOURCE_STATE_COPY_DEST);
    barrier.Transition.StateAfter = D3D12_RESOURCE_STATE_COPY_SOURCE;
    list_->ResourceBarrier(1, &barrier);
    for (UINT i = 0; i < count && readback; ++i) {
      D3D12_TEXTURE_COPY_LOCATION dst = {readback, D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT};
      dst.PlacedFootprint = layouts[i];
      D3D12_TEXTURE_COPY_LOCATION src = {entry.resource, D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX};
      src.SubresourceIndex = i;
      list_->CopyTextureRegion(&dst, 0, 0, 0, &src, nullptr);
    }
    barrier.Transition.StateBefore = D3D12_RESOURCE_STATE_COPY_SOURCE;
  }
  barrier.Transition.StateAfter =
      D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE | D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE;
  list_->ResourceBarrier(1, &barrier);
  const bool ok = Execute(list_);
  upload->Release();

  if (readback) {
    uint8_t* back = nullptr;
    readback->Map(0, nullptr, reinterpret_cast<void**>(&back));
    for (UINT i = 0; i < count; ++i) {
      const auto& sub = texture.subresources[i];
      bool same = true;
      for (UINT row = 0; row < rows[i] && row < sub.rows && same; ++row)
        same = !std::memcmp(back + layouts[i].Offset + size_t(row) * layouts[i].Footprint.RowPitch,
                            sub.data.data() + size_t(row) * sub.row_pitch,
                            std::min<size_t>(sub.row_pitch, size_t(row_sizes[i])));
      ++checked_;
      if (!same) ++mismatched_;
    }
    readback->Unmap(0, nullptr);
    readback->Release();
  }
  return ok;
}

}  // namespace blitz
