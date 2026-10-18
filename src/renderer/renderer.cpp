// Native renderer phase N5; see renderer.h. This file: setup, the frame, the
// register file, shaders, render targets, clears, resolves and the present.
// The draw path is in renderer_draw.cpp.

#include "renderer.h"

#include <algorithm>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <semaphore>
#include <thread>

#include <rex/cvar.h>
#include <rex/logging.h>
#include <rex/memory/utils.h>
#include <rex/ui/d3d12/d3d12_presenter.h>
#include <rex/ui/d3d12/d3d12_provider.h>
#include <rex/ui/d3d12/d3d12_util.h>

#include "shader_container.h"
#include "shaders/present_cs.h"
#include "shaders/resolve_depth_cs.h"
#include "shaders/resolve_depth_ms_cs.h"
#include "texture_cache.h"

REXCVAR_DECLARE(int32_t, trace_native_frame);
REXCVAR_DECLARE(std::string, screenshot_frames);
REXCVAR_DEFINE_STRING(dump_resolves, "", "Blitz",
                      "Dev: path prefix; in the frames of --screenshot_frames, every resolve's "
                      "destination is read back to <prefix>-f<frame>-<n>.res (tools/compare_resolves.py)");

REXCVAR_DEFINE_STRING(native_shader_dir, "", "Blitz",
                      "Directory with the translated shaders (<container>.dxil); shaders missing there "
                      "are translated at first use and saved there");

namespace blitz {

namespace {

namespace d3d12_util = rex::ui::d3d12::util;
constexpr size_t kUploadChunk = 64 * 1024 * 1024;
constexpr uint32_t kViewHeapSize = 65536, kSamplerHeapSize = 2048;

uint32_t Load(const uint8_t* base, uint32_t address) {
  return rex::memory::load_and_swap<uint32_t>(base + address);
}

D3D12_CPU_DESCRIPTOR_HANDLE Cpu(ID3D12DescriptorHeap* heap, uint32_t increment, uint32_t index) {
  auto handle = heap->GetCPUDescriptorHandleForHeapStart();
  handle.ptr += size_t(increment) * index;
  return handle;
}

ID3D12DescriptorHeap* CreateHeap(ID3D12Device* device, D3D12_DESCRIPTOR_HEAP_TYPE type,
                                 uint32_t count, bool shader_visible) {
  D3D12_DESCRIPTOR_HEAP_DESC desc = {type, count,
                                     shader_visible ? D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE
                                                    : D3D12_DESCRIPTOR_HEAP_FLAG_NONE, 0};
  ID3D12DescriptorHeap* heap = nullptr;
  device->CreateDescriptorHeap(&desc, IID_PPV_ARGS(&heap));
  return heap;
}

// Runs a tool without a console window, below the game's priority. True if it exits with 0.
bool RunTool(std::wstring command) {
  STARTUPINFOW startup = {sizeof(startup)};
  PROCESS_INFORMATION process = {};
  if (!CreateProcessW(nullptr, command.data(), nullptr, nullptr, FALSE,
                      CREATE_NO_WINDOW | BELOW_NORMAL_PRIORITY_CLASS, nullptr, nullptr, &startup, &process))
    return false;
  DWORD code = 1;
  if (WaitForSingleObject(process.hProcess, 120000) == WAIT_OBJECT_0)
    GetExitCodeProcess(process.hProcess, &code);
  else
    TerminateProcess(process.hProcess, 1);
  CloseHandle(process.hThread);
  CloseHandle(process.hProcess);
  return code == 0;
}

// What tools/translate_shaders.py does for one container: save it as
// <name>.xsc, translate it with XenosRecomp, compile with dxc (as a library if
// it uses specialization constants), and return the DXIL, also saved as
// <name>.dxil. Empty on failure (dxc's messages stay in <name>.dxc.log).
std::vector<uint8_t> TranslateShader(const std::filesystem::path& dir, const std::string& name,
                                     const std::string& container) {
  // A few at a time, so a burst of new shaders leaves the game its cores.
  static std::counting_semaphore<> slots(std::max(1u, std::thread::hardware_concurrency() / 4));
  slots.acquire();
  const auto start = std::chrono::steady_clock::now();
  const auto file = [&](const char* extension) { return dir / (name + extension); };
  const auto quote = [](const std::filesystem::path& path) { return L"\"" + path.wstring() + L"\""; };
  std::ofstream(file(".xsc"), std::ios::binary).write(container.data(), container.size());
  bool ok = RunTool(quote(BLITZ_XENOS_RECOMP) + L" " + quote(file(".xsc")) + L" " + quote(file(".hlsl")) +
                    L" " + quote(BLITZ_SHADER_COMMON));
  const wchar_t* target = L"";
  if (ok) {
    // Written under a temporary name so an interrupted compile never leaves a bad cache entry.
    for (const wchar_t* t : {name.ends_with(".vs") ? L"vs_6_0" : L"ps_6_0", L"lib_6_3"}) {
      target = t;
      ok = RunTool(quote(BLITZ_DXC) + L" -T " + t +
                   L" -HV 2021 -all-resources-bound -Wno-ignored-attributes -Qstrip_reflect -Qstrip_debug -Fo " +
                   quote(file(".dxil.tmp")) + L" -Fe " + quote(file(".dxc.log")) + L" " + quote(file(".hlsl")));
      if (ok) break;
    }
  }
  std::error_code error;
  if (ok) std::filesystem::rename(file(".dxil.tmp"), file(".dxil"), error);
  std::vector<uint8_t> dxil;
  if (ok && !error) {
    std::ifstream in(file(".dxil"), std::ios::binary);
    dxil.assign(std::istreambuf_iterator<char>(in), {});
    std::filesystem::remove(file(".dxc.log"), error);
  }
  slots.release();
  const double seconds = std::chrono::duration<double>(std::chrono::steady_clock::now() - start).count();
  if (dxil.empty())
    REXLOG_WARN("native_renderer: could not translate shader {} ({:.1f} s): {}", name, seconds,
                *target ? "dxc failed, see " + file(".dxc.log").string() : std::string("XenosRecomp failed"));
  else
    REXLOG_INFO("native_renderer: translated shader {} ({}, {:.1f} s)", name,
                std::filesystem::path(target).string(), seconds);
  return dxil;
}

}  // namespace

NativeRenderer::NativeRenderer(const rex::ui::d3d12::D3D12Provider& provider,
                               rex::ui::Presenter* presenter, const uint8_t* physical,
                               TextureCache* textures)
    : provider_(provider), presenter_(presenter), physical_(physical), textures_(textures),
      regs_(0x5003, 0) {
  device_ = provider.GetDevice();
  queue_ = provider.GetDirectQueue();
}

NativeRenderer::~NativeRenderer() {
  if (recording_) list_->Close();  // the queue may already be gone at exit
  for (auto& [key, target] : targets_)
    if (target.resource) target.resource->Release();
  for (auto& [key, pipeline] : pipelines_)
    if (pipeline) pipeline->Release();
  for (auto* buffer : upload_buffers_) buffer->Release();
  for (auto* object : {static_cast<IUnknown*>(root_signature_), static_cast<IUnknown*>(present_root_signature_),
                       static_cast<IUnknown*>(present_pipeline_), static_cast<IUnknown*>(view_heap_),
                       static_cast<IUnknown*>(depth_copy_root_signature_),
                       static_cast<IUnknown*>(depth_copy_pipeline_),
                       static_cast<IUnknown*>(depth_copy_ms_pipeline_), static_cast<IUnknown*>(list1_),
                       static_cast<IUnknown*>(sampler_heap_), static_cast<IUnknown*>(rtv_heap_),
                       static_cast<IUnknown*>(dsv_heap_), static_cast<IUnknown*>(list_),
                       static_cast<IUnknown*>(allocator_), static_cast<IUnknown*>(fence_)})
    if (object) object->Release();
  if (fence_event_) CloseHandle(fence_event_);
}

bool NativeRenderer::Initialize() {
  // The bindings XenosRecomp's shaders declare: constants b0 (VS), b1 (PS), b2
  // (shared) and b3 (specialization) in space4; bindless 2D/3D/cube textures
  // at t0 in spaces 0-2 and samplers at s0 in space 3.
  D3D12_DESCRIPTOR_RANGE ranges[4] = {};
  for (int i = 0; i < 3; ++i)
    ranges[i] = {D3D12_DESCRIPTOR_RANGE_TYPE_SRV, UINT_MAX, 0, UINT(i), 0};
  ranges[3] = {D3D12_DESCRIPTOR_RANGE_TYPE_SAMPLER, UINT_MAX, 0, 3, 0};
  D3D12_ROOT_PARAMETER params[13] = {};
  for (int i = 0; i < 3; ++i) {
    params[i].ParameterType = D3D12_ROOT_PARAMETER_TYPE_CBV;
    params[i].Descriptor = {UINT(i), 4};
    params[i].ShaderVisibility = i == 0 ? D3D12_SHADER_VISIBILITY_VERTEX
                                 : i == 1 ? D3D12_SHADER_VISIBILITY_PIXEL
                                          : D3D12_SHADER_VISIBILITY_ALL;
  }
  params[3].ParameterType = D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS;
  params[3].Constants = {3, 4, 1};
  params[3].ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;
  for (int i = 0; i < 4; ++i) {
    params[4 + i].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
    params[4 + i].DescriptorTable = {1, &ranges[i]};
    params[4 + i].ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;
  }
  // Vertex fetch (BlitzVertexFetch): descriptors at b4 space4, raw streams at t0-t3 space5.
  params[8].ParameterType = D3D12_ROOT_PARAMETER_TYPE_CBV;
  params[8].Descriptor = {4, 4};
  params[8].ShaderVisibility = D3D12_SHADER_VISIBILITY_VERTEX;
  for (int i = 0; i < 4; ++i) {
    params[9 + i].ParameterType = D3D12_ROOT_PARAMETER_TYPE_SRV;
    params[9 + i].Descriptor = {UINT(i), 5};
    params[9 + i].ShaderVisibility = D3D12_SHADER_VISIBILITY_VERTEX;
  }
  D3D12_ROOT_SIGNATURE_DESC desc = {13, params, 0, nullptr, D3D12_ROOT_SIGNATURE_FLAG_NONE};
  ID3DBlob* blob = nullptr;
  ID3DBlob* errors = nullptr;
  if (FAILED(provider_.SerializeRootSignature(&desc, D3D_ROOT_SIGNATURE_VERSION_1, &blob, &errors)) ||
      FAILED(device_->CreateRootSignature(0, blob->GetBufferPointer(), blob->GetBufferSize(),
                                          IID_PPV_ARGS(&root_signature_)))) {
    REXLOG_ERROR("native_renderer: root signature: {}",
                 errors ? static_cast<const char*>(errors->GetBufferPointer()) : "?");
    return false;
  }
  blob->Release();

  // Present: frontbuffer SRV (t0) and guest output UAV (u0), heap slots 0 and 1.
  D3D12_DESCRIPTOR_RANGE present_ranges[2] = {{D3D12_DESCRIPTOR_RANGE_TYPE_SRV, 1, 0, 0, 0},
                                              {D3D12_DESCRIPTOR_RANGE_TYPE_UAV, 1, 0, 0, 1}};
  D3D12_ROOT_PARAMETER present_params[2] = {};
  present_params[0].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
  present_params[0].DescriptorTable = {2, present_ranges};
  present_params[1].ParameterType = D3D12_ROOT_PARAMETER_TYPE_CBV;  // gamma ramp, b0
  D3D12_ROOT_SIGNATURE_DESC present_desc = {2, present_params, 0, nullptr,
                                            D3D12_ROOT_SIGNATURE_FLAG_NONE};
  if (FAILED(provider_.SerializeRootSignature(&present_desc, D3D_ROOT_SIGNATURE_VERSION_1, &blob,
                                              nullptr)) ||
      FAILED(device_->CreateRootSignature(0, blob->GetBufferPointer(), blob->GetBufferSize(),
                                          IID_PPV_ARGS(&present_root_signature_))))
    return false;
  blob->Release();
  D3D12_COMPUTE_PIPELINE_STATE_DESC compute = {};
  compute.pRootSignature = present_root_signature_;
  compute.CS = {g_present_cs, sizeof(g_present_cs)};
  if (FAILED(device_->CreateComputePipelineState(&compute, IID_PPV_ARGS(&present_pipeline_))))
    return false;

  // Depth resolve: depth plane SRV (t0), destination UAV (u0), rect constants (b0).
  D3D12_DESCRIPTOR_RANGE copy_ranges[2] = {{D3D12_DESCRIPTOR_RANGE_TYPE_SRV, 1, 0, 0, 0},
                                           {D3D12_DESCRIPTOR_RANGE_TYPE_UAV, 1, 0, 0, 0}};
  D3D12_ROOT_PARAMETER copy_params[3] = {};
  for (int i = 0; i < 2; ++i) {
    copy_params[i].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
    copy_params[i].DescriptorTable = {1, &copy_ranges[i]};
  }
  copy_params[2].ParameterType = D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS;
  copy_params[2].Constants = {0, 0, 6};
  D3D12_ROOT_SIGNATURE_DESC copy_desc = {3, copy_params, 0, nullptr, D3D12_ROOT_SIGNATURE_FLAG_NONE};
  if (FAILED(provider_.SerializeRootSignature(&copy_desc, D3D_ROOT_SIGNATURE_VERSION_1, &blob, nullptr)) ||
      FAILED(device_->CreateRootSignature(0, blob->GetBufferPointer(), blob->GetBufferSize(),
                                          IID_PPV_ARGS(&depth_copy_root_signature_))))
    return false;
  blob->Release();
  compute.pRootSignature = depth_copy_root_signature_;
  compute.CS = {g_resolve_depth_cs, sizeof(g_resolve_depth_cs)};
  if (FAILED(device_->CreateComputePipelineState(&compute, IID_PPV_ARGS(&depth_copy_pipeline_))))
    return false;
  compute.CS = {g_resolve_depth_ms_cs, sizeof(g_resolve_depth_ms_cs)};
  if (FAILED(device_->CreateComputePipelineState(&compute, IID_PPV_ARGS(&depth_copy_ms_pipeline_))))
    return false;

  view_heap_ = CreateHeap(device_, D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV, kViewHeapSize, true);
  sampler_heap_ = CreateHeap(device_, D3D12_DESCRIPTOR_HEAP_TYPE_SAMPLER, kSamplerHeapSize, true);
  rtv_heap_ = CreateHeap(device_, D3D12_DESCRIPTOR_HEAP_TYPE_RTV, 256, false);
  dsv_heap_ = CreateHeap(device_, D3D12_DESCRIPTOR_HEAP_TYPE_DSV, 64, false);
  view_increment_ = device_->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
  sampler_increment_ = device_->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_SAMPLER);
  rtv_increment_ = device_->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_RTV);
  dsv_increment_ = device_->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_DSV);
  if (!view_heap_ || !sampler_heap_ || !rtv_heap_ || !dsv_heap_) return false;
  D3D12_SHADER_RESOURCE_VIEW_DESC null_view = {};
  null_view.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
  null_view.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
  null_view.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
  device_->CreateShaderResourceView(nullptr, &null_view, Cpu(view_heap_, view_increment_, kNullTexture2D));
  null_view.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE3D;
  device_->CreateShaderResourceView(nullptr, &null_view, Cpu(view_heap_, view_increment_, kNullTexture3D));
  null_view.ViewDimension = D3D12_SRV_DIMENSION_TEXTURECUBE;
  device_->CreateShaderResourceView(nullptr, &null_view, Cpu(view_heap_, view_increment_, kNullTextureCube));
  D3D12_SAMPLER_DESC sampler = {D3D12_FILTER_MIN_MAG_MIP_LINEAR, D3D12_TEXTURE_ADDRESS_MODE_WRAP,
                                D3D12_TEXTURE_ADDRESS_MODE_WRAP, D3D12_TEXTURE_ADDRESS_MODE_WRAP};
  sampler.MaxAnisotropy = 1;
  sampler.MaxLOD = D3D12_FLOAT32_MAX;
  device_->CreateSampler(&sampler, Cpu(sampler_heap_, sampler_increment_, 0));

  if (FAILED(device_->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT,
                                             IID_PPV_ARGS(&allocator_))) ||
      FAILED(device_->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, allocator_, nullptr,
                                        IID_PPV_ARGS(&list_))) ||
      FAILED(device_->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&fence_))) ||
      FAILED(list_->QueryInterface(IID_PPV_ARGS(&list1_))))  // for ResolveSubresourceRegion
    return false;
  list_->Close();
  fence_event_ = CreateEvent(nullptr, FALSE, FALSE, nullptr);
  if (REXCVAR_GET(native_shader_dir).empty())
    REXLOG_WARN("native_renderer: --native_shader_dir is not set; nothing will be drawn");
  REXLOG_INFO("native_renderer: D3D12 renderer ready");
  return true;
}

// ---------------------------------------------------------------------------
// Frame

bool NativeRenderer::BeginFrame() {
  if (recording_) return true;
  if (device_lost_) return false;
  if (FAILED(allocator_->Reset()) || FAILED(list_->Reset(allocator_, nullptr))) return false;
  ID3D12DescriptorHeap* heaps[] = {view_heap_, sampler_heap_};
  list_->SetDescriptorHeaps(2, heaps);
  list_->SetGraphicsRootSignature(root_signature_);
  recording_ = true;
  return true;
}

void NativeRenderer::Submit() {
  if (!recording_) return;
  recording_ = false;
  list_->Close();
  ID3D12CommandList* lists[] = {list_};
  queue_->ExecuteCommandLists(1, lists);
  queue_->Signal(fence_, ++fence_value_);
  if (fence_->GetCompletedValue() < fence_value_) {
    fence_->SetEventOnCompletion(fence_value_, fence_event_);
    // A GPU fault would otherwise hang the game here; stop drawing instead.
    if (WaitForSingleObject(fence_event_, 5000) != WAIT_OBJECT_0) {
      REXLOG_ERROR("native_renderer: GPU timeout, device status {:08X}; drawing stops",
                   uint32_t(device_->GetDeviceRemovedReason()));
      device_lost_ = true;
    }
  }
  for (auto* resource : frame_garbage_) resource->Release();
  frame_garbage_.clear();
  frame_streams_.clear();
  upload_offset_ = 0;
  max_frame_upload_bytes_ = std::max(max_frame_upload_bytes_, frame_upload_bytes_);
  frame_upload_bytes_ = 0;
}

D3D12_GPU_VIRTUAL_ADDRESS NativeRenderer::Upload(const void* data, size_t size, size_t alignment,
                                                 void** mapped) {
  size_t offset = (upload_offset_ + alignment - 1) & ~(alignment - 1);
  if (upload_buffers_.empty() || offset + size > upload_size_) {
    // Keep the full chunk alive until the GPU is done with this frame.
    if (!upload_buffers_.empty()) frame_garbage_.push_back(upload_buffers_.back()), upload_buffers_.pop_back();
    upload_size_ = std::max(kUploadChunk, size);
    D3D12_RESOURCE_DESC desc = {};
    desc.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
    desc.Width = upload_size_;
    desc.Height = desc.DepthOrArraySize = desc.MipLevels = 1;
    desc.SampleDesc.Count = 1;
    desc.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
    ID3D12Resource* buffer = nullptr;
    device_->CreateCommittedResource(&d3d12_util::kHeapPropertiesUpload, D3D12_HEAP_FLAG_NONE, &desc,
                                     D3D12_RESOURCE_STATE_GENERIC_READ, nullptr, IID_PPV_ARGS(&buffer));
    buffer->Map(0, nullptr, reinterpret_cast<void**>(&upload_mapped_));
    upload_buffers_.push_back(buffer);
    offset = 0;
  }
  if (data) std::memcpy(upload_mapped_ + offset, data, size);
  if (mapped) *mapped = upload_mapped_ + offset;
  upload_bytes_ += size;
  frame_upload_bytes_ += size;
  upload_offset_ = offset + size;
  return upload_buffers_.back()->GetGPUVirtualAddress() + offset;
}

bool NativeRenderer::DumpingResolves() {
  if (REXCVAR_GET(dump_resolves).empty()) return false;
  // The frame being drawn is the one after the last presented.
  const std::string list = "," + REXCVAR_GET(screenshot_frames) + ",";
  return list.find("," + std::to_string(frames_ + 1) + ",") != std::string::npos;
}

void NativeRenderer::ReadBackResolve(TextureCache::Entry& dest, const uint32_t fetch[6]) {
  D3D12_RESOURCE_DESC desc = dest.resource->GetDesc();
  ResolveDump dump{};
  std::copy_n(fetch, 6, dump.fetch);
  device_->GetCopyableFootprints(&desc, 0, 1, 0, &dump.footprint, nullptr, nullptr, &dump.size);
  D3D12_RESOURCE_DESC buffer_desc = {};
  buffer_desc.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
  buffer_desc.Width = dump.size;
  buffer_desc.Height = buffer_desc.DepthOrArraySize = buffer_desc.MipLevels = 1;
  buffer_desc.SampleDesc.Count = 1;
  buffer_desc.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
  if (FAILED(device_->CreateCommittedResource(&d3d12_util::kHeapPropertiesReadback, D3D12_HEAP_FLAG_NONE,
                                              &buffer_desc, D3D12_RESOURCE_STATE_COPY_DEST, nullptr,
                                              IID_PPV_ARGS(&dump.buffer))))
    return;
  Transition(dest.resource, dest.state, D3D12_RESOURCE_STATE_COPY_SOURCE);
  D3D12_TEXTURE_COPY_LOCATION dst = {dump.buffer, D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT};
  dst.PlacedFootprint = dump.footprint;
  D3D12_TEXTURE_COPY_LOCATION src = {dest.resource, D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX};
  list_->CopyTextureRegion(&dst, 0, 0, 0, &src, nullptr);
  Transition(dest.resource, dest.state,
             D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE | D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
  dump.frame = uint32_t(frames_ + 1);
  resolve_dumps_.push_back(dump);
}

void NativeRenderer::WriteResolveDumps() {
  // After Submit: the copies are done. Header: fetch constant (6 dwords),
  // width, height, DXGI format, row pitch; then the rows.
  for (size_t i = 0; i < resolve_dumps_.size(); ++i) {
    auto& dump = resolve_dumps_[i];
    const std::string path = REXCVAR_GET(dump_resolves) + "-f" + std::to_string(dump.frame) + "-" +
                             std::to_string(i) + ".res";
    void* data = nullptr;
    if (FILE* f = std::fopen(path.c_str(), "wb"); f && SUCCEEDED(dump.buffer->Map(0, nullptr, &data))) {
      const uint32_t header[4] = {dump.footprint.Footprint.Width, dump.footprint.Footprint.Height,
                                  uint32_t(dump.footprint.Footprint.Format), dump.footprint.Footprint.RowPitch};
      std::fwrite(dump.fetch, 4, 6, f);
      std::fwrite(header, 4, 4, f);
      std::fwrite(data, 1, size_t(dump.footprint.Footprint.RowPitch) * dump.footprint.Footprint.Height, f);
      dump.buffer->Unmap(0, nullptr);
      std::fclose(f);
    }
    dump.buffer->Release();
  }
  resolve_dumps_.clear();
}

D3D12_GPU_VIRTUAL_ADDRESS NativeRenderer::StreamAddress(uint32_t address, uint32_t size) {
  auto& va = frame_streams_[(uint64_t(address) << 32) | size];
  if (!va) va = Upload(physical_ + address, size, 256);
  return va;
}

void NativeRenderer::Transition(ID3D12Resource* resource, D3D12_RESOURCE_STATES& state,
                                D3D12_RESOURCE_STATES to) {
  if (state == to) return;
  D3D12_RESOURCE_BARRIER barrier = {};
  barrier.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
  barrier.Transition = {resource, D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES, state, to};
  list_->ResourceBarrier(1, &barrier);
  state = to;
}

void NativeRenderer::Skip(const std::string& reason) {
  ++skipped_;
  if (int32_t(frames_) == REXCVAR_GET(trace_native_frame)) REXLOG_INFO("trace_native: skip: {}", reason);
  if (skip_reasons_[reason]++ == 0) REXLOG_WARN("native_renderer: skipping: {}", reason);
}

void NativeRenderer::EndFrame(uint32_t frontbuffer, uint8_t* base) {
  ++frames_;
  const auto now = std::chrono::steady_clock::now();
  if (last_present_ != std::chrono::steady_clock::time_point{}) {
    const double ms = std::chrono::duration<double, std::milli>(now - last_present_).count();
    frame_ms_sum_ += ms;
    frame_ms_max_ = std::max(frame_ms_max_, ms);
    slow_frames_ += ms > 25.0;
  }
  last_present_ = now;
  if (frames_ % 600 == 0) {
    std::string reasons;
    for (auto& [reason, count] : skip_reasons_) reasons += " [" + reason + ": " + std::to_string(count) + "]";
    REXLOG_INFO("native_renderer: {} frames, {} draws, {} skipped{}; {} pipelines, {} targets; uploads {} MB "
                "(largest frame {} MB); frame time avg {:.2f} ms, max {:.1f} ms, {} over 25 ms", frames_, draws_,
                skipped_, reasons, pipelines_.size(), targets_.size(), upload_bytes_ >> 20,
                max_frame_upload_bytes_ >> 20, frame_ms_sum_ / 600, frame_ms_max_, slow_frames_);
    upload_bytes_ = max_frame_upload_bytes_ = 0;
    frame_ms_sum_ = frame_ms_max_ = 0;
    slow_frames_ = 0;
  }
  TextureCache::Entry* front = nullptr;
  if (frontbuffer) {
    uint32_t fetch[6];
    for (int i = 0; i < 6; ++i) fetch[i] = Load(base, frontbuffer + 28 + 4 * i);
    fetch[1] = (fetch[1] & 0xFFF) | PhysicalAddress(fetch[1] & 0xFFFFF000);
    if (fetch[5] & 0xFFFFF000) fetch[5] = (fetch[5] & 0xFFF) | PhysicalAddress(fetch[5] & 0xFFFFF000);
    front = textures_->Find(fetch);
  }
  if (!front || !presenter_) {
    if (frontbuffer) Skip("frontbuffer was never resolved");
    Submit();
    return;
  }
  if (!BeginFrame()) return;
  presenter_->RefreshGuestOutput(
      front->width, front->height, 1280, 720,
      [&](rex::ui::Presenter::GuestOutputRefreshContext& context) -> bool {
        auto& d3d12_context =
            static_cast<rex::ui::d3d12::D3D12Presenter::D3D12GuestOutputRefreshContext&>(context);
        ID3D12Resource* output = d3d12_context.resource_uav_capable();
        context.SetIs8bpc(true);
        D3D12_SHADER_RESOURCE_VIEW_DESC srv = {};
        srv.Format = front->format;
        srv.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
        srv.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
        srv.Texture2D.MipLevels = 1;
        device_->CreateShaderResourceView(front->resource, &srv, Cpu(view_heap_, view_increment_, 0));
        D3D12_UNORDERED_ACCESS_VIEW_DESC uav = {};
        uav.Format = rex::ui::d3d12::D3D12Presenter::kGuestOutputFormat;
        uav.ViewDimension = D3D12_UAV_DIMENSION_TEXTURE2D;
        device_->CreateUnorderedAccessView(output, nullptr, &uav, Cpu(view_heap_, view_increment_, 1));
        D3D12_RESOURCE_STATES output_state = rex::ui::d3d12::D3D12Presenter::kGuestOutputInternalState;
        Transition(output, output_state, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
        Transition(front->resource, front->state, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE |
                                                      D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
        list_->SetComputeRootSignature(present_root_signature_);
        list_->SetPipelineState(present_pipeline_);
        list_->SetComputeRootDescriptorTable(0, view_heap_->GetGPUDescriptorHandleForHeapStart());
        static GammaRamp::Constants gamma;  // 4 KB; the render thread's only
        if (gamma_source_) gamma_source_(gamma);
        else GammaRamp().Get(nullptr, gamma);  // linear
        list_->SetComputeRootConstantBufferView(1, Upload(&gamma, sizeof(gamma), 256));
        list_->Dispatch((front->width + 7) / 8, (front->height + 7) / 8, 1);
        Transition(output, output_state, rex::ui::d3d12::D3D12Presenter::kGuestOutputInternalState);
        Submit();
        return true;
      });
  Submit();  // in case the presenter skipped the refresh
  WriteResolveDumps();
}

// ---------------------------------------------------------------------------
// State

void NativeRenderer::ApplyFlush(uint32_t function, const PPCContext& ctx, uint8_t* base) {
  const uint64_t mask = ctx.r4.u64;
  // Mask bit 63 is the first block; per_bit registers per bit, from `src`.
  auto apply = [&](uint32_t first, uint32_t src, uint32_t per_bit) {
    for (uint32_t i = 0; i < 64; ++i) {
      if (!((mask >> (63 - i)) & 1)) continue;
      for (uint32_t k = 0; k < per_bit; ++k) {
        const uint32_t reg = first + i * per_bit + k;
        if (reg < regs_.size()) regs_[reg] = Load(base, src + 4 * (i * per_bit + k));
      }
    }
  };
  switch (function) {
    case d3d::kFlushRegisters: apply(ctx.r5.u32, ctx.r6.u32, 1); break;
    case d3d::kFlushFloatConstants: apply(ctx.r5.u32, ctx.r6.u32, 16); break;
    case d3d::kFlushFetchConstants: apply(0x4800, ctx.r3.u32 + d3d::kDeviceFetchConstants, 6); break;
    case d3d::kFlushClipPlanes: apply(0x2388, ctx.r3.u32 + 10272, 4); break;
    default: break;
  }
}

NativeRenderer::Shader* NativeRenderer::ShaderFor(uint32_t object, uint8_t* base) {
  if (!object) return nullptr;
  auto known = shader_objects_.find(object);
  if (known != shader_objects_.end()) return known->second;
  auto parts = registered_.find(object);
  if (parts == registered_.end()) return nullptr;
  const std::string container = ReadContainer(base, physical_, parts->second.first, parts->second.second);
  if (container.empty()) return nullptr;
  const std::string name = ContainerName(container);
  auto& shader = shaders_[name];
  if (!shader) {
    shader = std::make_unique<Shader>();
    shader->name = name;
    shader->vertex = name.ends_with(".vs");
    shader->elements = VertexElements(container);
    const auto path = std::filesystem::path(REXCVAR_GET(native_shader_dir)) / (name + ".dxil");
    std::ifstream file(path, std::ios::binary);
    if (file) {
      shader->dxil.assign(std::istreambuf_iterator<char>(file), {});
      shader->loaded = !shader->dxil.empty();
    }
    if (!shader->loaded && !REXCVAR_GET(native_shader_dir).empty()) {
      REXLOG_INFO("native_renderer: no translated shader {}; translating it", name);
      shader->translating = std::async(std::launch::async, TranslateShader,
                                       std::filesystem::path(REXCVAR_GET(native_shader_dir)), name, container);
    }
  }
  shader_objects_[object] = shader.get();
  return shader.get();
}

void NativeRenderer::ReadBoundVertexShader(uint32_t from, uint32_t to, uint8_t* base) {
  // D3D_FlushShaders loads the microcode the GPU will run (for vertex shaders,
  // the copy patched for the vertex declaration): IM_LOAD from memory, or
  // IM_LOAD_IMMEDIATE with the code in the packet (inline-vertex strides).
  // (from, to]: the command pointer points at the last dword written.
  if (to < from || to - from > (1u << 20)) {
    Skip("shader flush range not understood");
    return;
  }
  auto load_vertex_shader = [&](const uint8_t* memory, uint32_t address, uint32_t dwords) {
    vs_code_.resize(dwords);
    vs_code_hash_ = 0xCBF29CE484222325ull;
    for (uint32_t i = 0; i < dwords; ++i) {
      vs_code_[i] = Load(memory, address + 4 * i);
      vs_code_hash_ = (vs_code_hash_ ^ vs_code_[i]) * 0x100000001B3ull;
    }
  };
  for (uint32_t at = from + 4; at <= to;) {
    const uint32_t header = Load(base, at);
    const uint32_t count = ((header >> 16) & 0x3FFF) + 1;
    switch (header >> 30) {
      case 0: at += 4 * (count + 1); break;
      case 1: at += 12; break;
      case 2: at += 4; break;
      default: {
        const uint32_t opcode = (header >> 8) & 0x7F;
        if (opcode == 0x27 && count >= 2) {  // IM_LOAD: address | type, start | size
          const uint32_t address = Load(base, at + 4), dwords = Load(base, at + 8) & 0xFFFF;
          if ((address & 3) == 0 && uint64_t(address) + 4 * dwords <= 0x20000000)
            load_vertex_shader(physical_, address, dwords);
        } else if (opcode == 0x2B && count >= 2) {  // IM_LOAD_IMMEDIATE: type, start | size, code
          const uint32_t dwords = std::min(Load(base, at + 8) & 0xFFFF, count - 2);
          if (Load(base, at + 4) == 0) load_vertex_shader(base, at + 12, dwords);
        }
        at += 4 * (count + 1);
        break;
      }
    }
  }
}

bool NativeRenderer::FirstRegisterWrite(const std::vector<std::pair<uint32_t, uint32_t>>& ranges, uint32_t reg,
                                        uint8_t* base, uint32_t* value) {
  for (auto [from, to] : ranges) {
    if (to < from || to - from > (1u << 20)) continue;
    for (uint32_t at = from + 4; at <= to;) {
      const uint32_t header = Load(base, at);
      const uint32_t count = ((header >> 16) & 0x3FFF) + 1;
      switch (header >> 30) {
        case 0: {  // type 0: registers from header bits 0-14 (bit 15: all to one)
          const uint32_t first = header & 0x7FFF;
          for (uint32_t i = 0; i < count; ++i) {
            if ((header & 0x8000 ? first : first + i) == reg) {
              *value = Load(base, at + 4 + 4 * i);
              return true;
            }
          }
          at += 4 * (count + 1);
          break;
        }
        case 1: at += 12; break;
        case 2: at += 4; break;
        default: at += 4 * (count + 1); break;
      }
    }
  }
  return false;
}

NativeRenderer::RenderTarget* NativeRenderer::Target(bool depth, uint32_t edram_base,
                                                     uint32_t format) {
  // The screen scissor is always 8192 and the window scissor never reaches the
  // shadow, so size by the surface: its pitch, and as many rows as EDRAM holds
  // at that pitch (2048 tiles of 80x16 samples), so color and depth match.
  // shortcut: a target as tall as EDRAM allows; size by use if memory matters.
  const uint32_t pitch = std::max(regs_[0x2000] & 0x3FFF, 1u);
  const uint32_t width = Tiled() ? tiled_width_ : pitch;
  const uint32_t height = Tiled() ? tiled_height_ : std::min(2048u * 1280u / pitch, 8192u);
  const uint32_t samples = 1u << std::min((regs_[0x2000] >> 16) & 3, 2u);  // RB_SURFACE_INFO msaa_samples
  const uint64_t key = (uint64_t(depth) << 63) | (uint64_t(edram_base & 0xFFF) << 48) |
                       (uint64_t(format & 0xF) << 44) | (uint64_t(samples) << 32) |
                       (uint64_t(width & 0x3FFF) << 16) | (height & 0xFFFF);
  auto it = targets_.find(key);
  if (it != targets_.end()) return &it->second;
  RenderTarget& target = targets_[key];
  target.depth = depth;
  target.width = width;
  target.height = height;
  target.samples = samples;
  target.format = depth ? DXGI_FORMAT_D32_FLOAT_S8X24_UINT : DXGI_FORMAT_R8G8B8A8_UNORM;
  if (!depth && format != 0 && format != 1)
    REXLOG_WARN("native_renderer: color format {} drawn as 8888", format);
  D3D12_RESOURCE_DESC desc = {};
  desc.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
  desc.Width = width;
  desc.Height = height;
  desc.DepthOrArraySize = 1;
  desc.MipLevels = 1;
  desc.Format = target.format;
  // shortcut: the standard D3D12 sample positions, not the Xenos ones; they only move AA edges.
  desc.SampleDesc.Count = samples;
  desc.Flags = depth ? D3D12_RESOURCE_FLAG_ALLOW_DEPTH_STENCIL : D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET;
  D3D12_CLEAR_VALUE clear = {target.format};
  if (depth) clear.DepthStencil = {1.0f, 0};
  target.state = depth ? D3D12_RESOURCE_STATE_DEPTH_WRITE : D3D12_RESOURCE_STATE_RENDER_TARGET;
  if (FAILED(device_->CreateCommittedResource(&d3d12_util::kHeapPropertiesDefault, D3D12_HEAP_FLAG_NONE,
                                              &desc, target.state, &clear, IID_PPV_ARGS(&target.resource)))) {
    REXLOG_ERROR("native_renderer: cannot create a {}x{} target", width, height);
    targets_.erase(key);
    return nullptr;
  }
  if (depth) {
    target.view = Cpu(dsv_heap_, dsv_increment_, dsv_next_++);
    device_->CreateDepthStencilView(target.resource, nullptr, target.view);
  } else {
    target.view = Cpu(rtv_heap_, rtv_increment_, rtv_next_++);
    device_->CreateRenderTargetView(target.resource, nullptr, target.view);
  }
  REXLOG_INFO("native_renderer: {} target at EDRAM {} format {}: {}x{}, {}x MSAA", depth ? "depth" : "color",
              edram_base, format, width, height, samples);
  return &target;
}

NativeRenderer::RenderTarget* NativeRenderer::ColorTarget() {
  const uint32_t info = regs_[0x2001];
  return Target(false, info & 0xFFF, (info >> 16) & 0xF);
}

NativeRenderer::RenderTarget* NativeRenderer::DepthTarget() {
  const uint32_t info = regs_[0x2002];
  return Target(true, info & 0xFFF, (info >> 16) & 1);
}

// ---------------------------------------------------------------------------
// Clears and resolves

void NativeRenderer::DoClear(const d3d::ClearOp& op, bool screen_rect) {
  if (!BeginFrame()) return;
  if (int32_t(frames_) == REXCVAR_GET(trace_native_frame))
    REXLOG_INFO("trace_native: clear {:X} rect {} {} {} {} color {} {} {} {} z {}", op.flags, op.rect[0], op.rect[1],
                op.rect[2], op.rect[3], op.color[0], op.color[1], op.color[2], op.color[3], op.z);
  D3D12_RECT rect;
  const D3D12_RECT* rects = nullptr;
  // A ClearF rect while tiled is in tile space and runs for each tile.
  if (op.has_rect && (screen_rect || !Tiled())) {
    rect = {op.rect[0], op.rect[1], op.rect[2], op.rect[3]};
    rects = &rect;
  }
  if (op.flags & 0xF0) {
    if (RenderTarget* color = ColorTarget()) {
      Transition(color->resource, color->state, D3D12_RESOURCE_STATE_RENDER_TARGET);
      list_->ClearRenderTargetView(color->view, op.color, rects ? 1 : 0, rects);
    }
  }
  if (op.flags & 3) {
    if (RenderTarget* depth = DepthTarget()) {
      Transition(depth->resource, depth->state, D3D12_RESOURCE_STATE_DEPTH_WRITE);
      D3D12_CLEAR_FLAGS flags = D3D12_CLEAR_FLAGS(((op.flags & 2) ? D3D12_CLEAR_FLAG_DEPTH : 0) |
                                                  ((op.flags & 1) ? D3D12_CLEAR_FLAG_STENCIL : 0));
      list_->ClearDepthStencilView(depth->view, flags, op.z, uint8_t(op.stencil), rects ? 1 : 0, rects);
    }
  }
}

void NativeRenderer::BeginTiling(const PPCContext& ctx, uint8_t* base) {
  // (dev, flags, tile count, tile rects x1 y1 x2 y2, clear color, Z (f1),
  // stencil): the tiled pass starts by clearing its targets.
  tiled_width_ = tiled_height_ = 0;
  for (uint32_t i = 0; i < std::min(ctx.r5.u32, 16u); ++i) {
    tiled_width_ = std::max(tiled_width_, Load(base, ctx.r6.u32 + 16 * i + 8));
    tiled_height_ = std::max(tiled_height_, Load(base, ctx.r6.u32 + 16 * i + 12));
  }
  d3d::ClearOp clear{};
  clear.flags = 0x13;  // target 0, depth and stencil
  if (ctx.r7.u32)
    for (int i = 0; i < 4; ++i) {
      const uint32_t bits = Load(base, ctx.r7.u32 + 4 * i);
      std::memcpy(&clear.color[i], &bits, 4);
    }
  clear.z = float(ctx.f1.f64);
  clear.stencil = ctx.r9.u32;
  DoClear(clear);
}

void NativeRenderer::ResolveDepth(RenderTarget& source, TextureCache::Entry& dest, const D3D12_BOX& box,
                                  const int32_t point[2]) {
  if (!source.srv_slot) {
    source.srv_slot = view_next_++;
    D3D12_SHADER_RESOURCE_VIEW_DESC srv = {};
    srv.Format = DXGI_FORMAT_R32_FLOAT_X8X24_TYPELESS;  // the depth plane
    srv.ViewDimension = source.samples > 1 ? D3D12_SRV_DIMENSION_TEXTURE2DMS : D3D12_SRV_DIMENSION_TEXTURE2D;
    srv.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
    if (source.samples == 1) srv.Texture2D.MipLevels = 1;
    device_->CreateShaderResourceView(source.resource, &srv, Cpu(view_heap_, view_increment_, source.srv_slot));
  }
  uint32_t& uav_slot = uav_slots_[dest.resource];
  if (!uav_slot) {
    uav_slot = view_next_++;
    D3D12_UNORDERED_ACCESS_VIEW_DESC uav = {};
    uav.Format = DXGI_FORMAT_R32_FLOAT;
    uav.ViewDimension = D3D12_UAV_DIMENSION_TEXTURE2D;
    device_->CreateUnorderedAccessView(dest.resource, nullptr, &uav, Cpu(view_heap_, view_increment_, uav_slot));
  }
  Transition(source.resource, source.state, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
  Transition(dest.resource, dest.state, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
  list_->SetComputeRootSignature(depth_copy_root_signature_);
  // MSAA depth resolves take sample 0 (depth is not averaged).
  list_->SetPipelineState(source.samples > 1 ? depth_copy_ms_pipeline_ : depth_copy_pipeline_);
  auto gpu = [&](uint32_t slot) {
    auto handle = view_heap_->GetGPUDescriptorHandleForHeapStart();
    handle.ptr += uint64_t(view_increment_) * slot;
    return handle;
  };
  list_->SetComputeRootDescriptorTable(0, gpu(source.srv_slot));
  list_->SetComputeRootDescriptorTable(1, gpu(uav_slot));
  const uint32_t width = box.right - box.left, height = box.bottom - box.top;
  const uint32_t constants[6] = {box.left, box.top, uint32_t(point[0]), uint32_t(point[1]), width, height};
  list_->SetComputeRoot32BitConstants(2, 6, constants, 0);
  list_->Dispatch((width + 7) / 8, (height + 7) / 8, 1);
  Transition(dest.resource, dest.state,
             D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE | D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
}

void NativeRenderer::DoResolve(const d3d::ResolveOp& op) {
  if (!BeginFrame()) return;
  const bool depth = op.source == 4;
  RenderTarget* source = depth ? DepthTarget() : ColorTarget();
  if (!source || !op.dest_texture) return;
  if (int32_t(frames_) == REXCVAR_GET(trace_native_frame))
    REXLOG_INFO("trace_native: resolve {}x{} rect {} {} {} {} to {:08X} ({}x{}) at {} {}{}", source->width,
                source->height, op.rect[0], op.rect[1], op.rect[2], op.rect[3], op.dest_fetch[1] & ~0xFFFu,
                (op.dest_fetch[2] & 0x1FFF) + 1, ((op.dest_fetch[2] >> 13) & 0x1FFF) + 1, op.dest_point[0],
                op.dest_point[1], op.clear_color ? " +clear" : "");
  // Depth goes into an R32_FLOAT texture (the game's k_24_8_FLOAT), sampled as depth in X.
  // A swapping copy stores blue first; the texture's swizzle (zyxw) swaps back.
  TextureCache::Entry* dest = textures_->ResolveTarget(op.dest_fetch, depth ? DXGI_FORMAT_R32_FLOAT : source->format,
                                                       op.swap_rb && !depth);
  if (!dest) return;

  // Source rect: the call's, else the destination's size. During predicated
  // tiling the game resolves each tile with its screen rect and point (traced).
  int32_t rect[4] = {0, 0, int32_t(dest->width), int32_t(dest->height)};
  const int32_t point[2] = {op.dest_point[0], op.dest_point[1]};
  if (op.has_rect) std::copy(op.rect, op.rect + 4, rect);
  D3D12_BOX box = {UINT(std::max(rect[0], 0)), UINT(std::max(rect[1], 0)), 0,
                   UINT(std::min<int32_t>(rect[2], source->width)),
                   UINT(std::min<int32_t>(rect[3], source->height)), 1};
  box.right = std::min<UINT>(box.right, box.left + dest->width - std::min<UINT>(point[0], dest->width));
  box.bottom = std::min<UINT>(box.bottom, box.top + dest->height - std::min<UINT>(point[1], dest->height));
  if (box.right > box.left && box.bottom > box.top && depth) {
    ResolveDepth(*source, *dest, box, point);
  } else if (box.right > box.left && box.bottom > box.top) {
    if (source->samples > 1) {  // the resolve averages the samples
      Transition(source->resource, source->state, D3D12_RESOURCE_STATE_RESOLVE_SOURCE);
      Transition(dest->resource, dest->state, D3D12_RESOURCE_STATE_RESOLVE_DEST);
      D3D12_RECT src_rect = {LONG(box.left), LONG(box.top), LONG(box.right), LONG(box.bottom)};
      list1_->ResolveSubresourceRegion(dest->resource, 0, point[0], point[1], source->resource, 0, &src_rect,
                                       source->format, D3D12_RESOLVE_MODE_AVERAGE);
    } else {
      Transition(source->resource, source->state, D3D12_RESOURCE_STATE_COPY_SOURCE);
      Transition(dest->resource, dest->state, D3D12_RESOURCE_STATE_COPY_DEST);
      D3D12_TEXTURE_COPY_LOCATION dst = {dest->resource, D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX};
      D3D12_TEXTURE_COPY_LOCATION src = {source->resource, D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX};
      list_->CopyTextureRegion(&dst, point[0], point[1], 0, &src, &box);
    }
    Transition(dest->resource, dest->state,
               D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE | D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
  }
  if (DumpingResolves()) ReadBackResolve(*dest, op.dest_fetch);
  if (op.clear_color || op.clear_depth) {
    d3d::ClearOp clear{};
    clear.flags = (op.clear_color ? 0x10 : 0) | (op.clear_depth ? 3 : 0);
    clear.has_rect = true;
    std::copy(rect, rect + 4, clear.rect);
    std::copy(op.clear_color_value, op.clear_color_value + 4, clear.color);
    clear.z = op.clear_z;
    clear.stencil = op.clear_stencil;
    DoClear(clear, true);
  }
}

// ---------------------------------------------------------------------------
// Calls

void NativeRenderer::Before(uint32_t function, PPCContext& ctx, uint8_t* base) {
  Pending pending{function, 0};
  switch (function) {
    case d3d::kFlushRegisters:
    case d3d::kFlushFloatConstants:
    case d3d::kFlushFetchConstants:
    case d3d::kFlushClipPlanes:
      ApplyFlush(function, ctx, base);
      return;
    case d3d::kRegisterVertexShader:
      registered_[ctx.r3.u32] = {ctx.r3.u32 + 872, ctx.r4.u32};
      shader_objects_.erase(ctx.r3.u32);
      return;
    case d3d::kRegisterPixelShader:
      registered_[ctx.r3.u32] = {ctx.r3.u32 + 40, ctx.r4.u32};
      shader_objects_.erase(ctx.r3.u32);
      return;
    case d3d::kCreateShader:
      pending.out_ptr = ctx.r3.u32;  // the container
      break;
    case d3d::kFlushShaders:
      guest_device_ = ctx.r3.u32;
      bound_vs_ = Load(base, guest_device_ + d3d::kDeviceBoundVertexShader);
      bound_ps_ = Load(base, guest_device_ + d3d::kDeviceBoundPixelShader);
      pending.range_from = Load(base, guest_device_ + d3d::kDeviceCommandPtr);
      // Name them now: this flush patches a new vertex shader's fetches in place,
      // and the translated shaders were captured before that (shader_capture.cpp).
      ShaderFor(bound_vs_, base);
      ShaderFor(bound_ps_, base);
      break;
    case d3d::kDrawVertices:
      guest_device_ = ctx.r3.u32;
      pending.draw = Draw{ctx.r4.u32, false, ctx.r5.u32, ctx.r6.u32};
      break;
    case d3d::kDrawIndexedVertices: {
      guest_device_ = ctx.r3.u32;
      Draw draw{ctx.r4.u32, true, ctx.r6.u32, ctx.r7.u32};
      draw.base_vertex = int32_t(ctx.r5.u32);
      const uint32_t ib = Load(base, guest_device_ + d3d::kDeviceIndexBuffer);
      if (ib) {
        draw.index32 = Load(base, ib) >> 31;
        draw.index_address = PhysicalAddress(Load(base, ib + 24));
        draw.index_endian = draw.index32 ? 2 : 1;
      }
      pending.draw = draw;
      break;
    }
    case d3d::kBeginVertices:
      guest_device_ = ctx.r3.u32;
      pending.draw = Draw{ctx.r4.u32, false, 0, ctx.r5.u32};
      pending.draw.inline_stride = ctx.r6.u32;
      pending.draw.inline_size = ctx.r5.u32 * ctx.r6.u32;
      break;
    case d3d::kBeginIndexedVertices: {
      // (dev, prim, base vertex, vertex count, index count, index format,
      // stride, &indices, &vertices); the 9th argument is on the stack.
      guest_device_ = ctx.r3.u32;
      pending.draw = Draw{ctx.r4.u32, true, 0, ctx.r7.u32};
      pending.draw.base_vertex = int32_t(ctx.r5.u32);
      pending.draw.index32 = (ctx.r8.u32 & 4) != 0;
      pending.draw.inline_stride = ctx.r9.u32;
      pending.draw.inline_size = ctx.r6.u32 * ctx.r9.u32;
      pending.out_ptr = ctx.r10.u32;
      pending.vertex_out_ptr = Load(base, ctx.r1.u32 + 84);
      break;
    }
    case d3d::kEndVertices:
      if (inline_pending_) {
        inline_pending_ = false;
        DoDraw(inline_draw_, base);
      }
      return;
    case d3d::kResolve:
      pending.resolve = d3d::DecodeResolve(ctx, base);
      pending.range_from = Load(base, ctx.r3.u32 + d3d::kDeviceCommandPtr);
      break;
    case d3d::kClearF:
      pending.clear = d3d::DecodeClear(ctx, base);
      break;
    case d3d::kBeginTiling:
      BeginTiling(ctx, base);
      return;
    case d3d::kSwap:
      EndFrame(ctx.r4.u32, base);
      return;
    case d3d::kKickOff:  // (dev, ...): the segment so far ends here
      for (auto& p : pending_)
        if (p.function == d3d::kFlushShaders || p.function == d3d::kResolve)
          p.ranges.push_back({p.range_from, Load(base, ctx.r3.u32 + d3d::kDeviceCommandPtr)});
      return;
    default:
      return;
  }
  pending_.push_back(pending);
}

void NativeRenderer::After(uint32_t function, PPCContext& ctx, uint8_t* base) {
  // DrawVerticesUP is BeginVertices, a copy of the data, and its own end
  // (it never calls EndVertices); the copy is done by now.
  if (function == d3d::kKickOff) {  // calls in flight continue in the new segment
    for (auto& p : pending_) p.range_from = Load(base, guest_device_ + d3d::kDeviceCommandPtr);
    return;
  }
  if (function == d3d::kDrawVerticesUP && inline_pending_) {
    inline_pending_ = false;
    DoDraw(inline_draw_, base);
    return;
  }
  switch (function) {
    case d3d::kCreateShader: case d3d::kFlushShaders: case d3d::kDrawVertices:
    case d3d::kDrawIndexedVertices: case d3d::kBeginVertices: case d3d::kBeginIndexedVertices:
    case d3d::kResolve: case d3d::kClearF:
      break;
    default:
      return;
  }
  if (pending_.empty() || pending_.back().function != function) return;
  Pending pending = std::move(pending_.back());
  pending_.pop_back();
  if (function == d3d::kFlushShaders || function == d3d::kResolve)
    pending.ranges.push_back({pending.range_from, Load(base, guest_device_ + d3d::kDeviceCommandPtr)});
  switch (function) {
    case d3d::kCreateShader: {
      const uint32_t object = ctx.r3.u32;
      if (object && !(Load(base, pending.out_ptr) & 1)) {  // pixel; vertex ones get registered
        registered_[object] = {object + 40, Load(base, object + 24)};
        shader_objects_.erase(object);
      }
      break;
    }
    case d3d::kFlushShaders:
      for (auto [from, to] : pending.ranges) ReadBoundVertexShader(from, to, base);
      break;
    case d3d::kDrawVertices:
    case d3d::kDrawIndexedVertices:
      DoDraw(pending.draw, base);
      break;
    case d3d::kBeginVertices:
      // The game writes the vertices into the returned space, then calls EndVertices.
      if (ctx.r3.u32) {
        inline_draw_ = pending.draw;
        inline_draw_.inline_vertices = PhysicalAddress(ctx.r3.u32);
        inline_pending_ = true;
      }
      break;
    case d3d::kBeginIndexedVertices:
      // Returns an HRESULT; the game fills both spaces, then calls EndVertices.
      if (ctx.r3.u32 == 0) {
        inline_draw_ = pending.draw;
        inline_draw_.index_address = PhysicalAddress(Load(base, pending.out_ptr));
        inline_draw_.inline_vertices = PhysicalAddress(Load(base, pending.vertex_out_ptr));
        inline_pending_ = true;
      }
      break;
    case d3d::kResolve:
      // RB_COPY_DEST_INFO as the call set it for its copy (it resets it to 0 after).
      if (uint32_t info; FirstRegisterWrite(pending.ranges, 0x231B, base, &info))
        pending.resolve.swap_rb = (info >> 24) & 1;
      DoResolve(pending.resolve);
      break;
    case d3d::kClearF:
      DoClear(pending.clear);
      break;
    default:
      break;
  }
}

}  // namespace blitz
