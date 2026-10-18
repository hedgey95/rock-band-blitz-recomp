// Native renderer phase N5: the draw path (see renderer.h).

#include <algorithm>
#include <bit>
#include <cmath>
#include <cstring>

#include <rex/cvar.h>
#include <rex/graphics/registers.h>
#include <rex/logging.h>
#include <rex/memory/utils.h>

#include "renderer.h"
#include "texture_cache.h"

REXCVAR_DEFINE_INT32(trace_native_frame, -1, "Blitz", "Dev: log every native draw of this frame");

namespace blitz {

namespace {

namespace reg = rex::graphics::reg;
namespace xenos = rex::graphics::xenos;

template <typename T>
T Reg(const std::vector<uint32_t>& regs) {
  T value;
  value.value = regs[uint32_t(T::register_index)];
  return value;
}

float RegFloat(const std::vector<uint32_t>& regs, uint32_t index) {
  float value;
  std::memcpy(&value, &regs[index], 4);
  return value;
}

D3D12_BLEND Blend(xenos::BlendFactor factor) {
  switch (uint32_t(factor)) {
    case 0: return D3D12_BLEND_ZERO;
    case 1: return D3D12_BLEND_ONE;
    case 4: return D3D12_BLEND_SRC_COLOR;
    case 5: return D3D12_BLEND_INV_SRC_COLOR;
    case 6: return D3D12_BLEND_SRC_ALPHA;
    case 7: return D3D12_BLEND_INV_SRC_ALPHA;
    case 8: return D3D12_BLEND_DEST_COLOR;
    case 9: return D3D12_BLEND_INV_DEST_COLOR;
    case 10: return D3D12_BLEND_DEST_ALPHA;
    case 11: return D3D12_BLEND_INV_DEST_ALPHA;
    case 12: case 14: return D3D12_BLEND_BLEND_FACTOR;
    case 13: case 15: return D3D12_BLEND_INV_BLEND_FACTOR;
    case 16: return D3D12_BLEND_SRC_ALPHA_SAT;
    default: return D3D12_BLEND_ONE;
  }
}

// D3D12 alpha blending only takes the alpha variants of color factors.
D3D12_BLEND BlendAlpha(xenos::BlendFactor factor) {
  switch (D3D12_BLEND b = Blend(factor)) {
    case D3D12_BLEND_SRC_COLOR: return D3D12_BLEND_SRC_ALPHA;
    case D3D12_BLEND_INV_SRC_COLOR: return D3D12_BLEND_INV_SRC_ALPHA;
    case D3D12_BLEND_DEST_COLOR: return D3D12_BLEND_DEST_ALPHA;
    case D3D12_BLEND_INV_DEST_COLOR: return D3D12_BLEND_INV_DEST_ALPHA;
    default: return b;
  }
}

D3D12_BLEND_OP BlendOp(xenos::BlendOp op) {
  switch (uint32_t(op)) {
    case 1: return D3D12_BLEND_OP_SUBTRACT;
    case 2: return D3D12_BLEND_OP_MIN;
    case 3: return D3D12_BLEND_OP_MAX;
    case 4: return D3D12_BLEND_OP_REV_SUBTRACT;
    default: return D3D12_BLEND_OP_ADD;
  }
}

D3D12_COMPARISON_FUNC Compare(xenos::CompareFunction f) {
  return D3D12_COMPARISON_FUNC(uint32_t(f) + 1);  // same order, D3D12 starts at 1
}

D3D12_STENCIL_OP StencilOp(xenos::StencilOp op) { return D3D12_STENCIL_OP(uint32_t(op) + 1); }

D3D12_PRIMITIVE_TOPOLOGY_TYPE TopologyType(uint32_t prim) {
  switch (prim) {
    case 1: return D3D12_PRIMITIVE_TOPOLOGY_TYPE_POINT;
    case 2: case 3: return D3D12_PRIMITIVE_TOPOLOGY_TYPE_LINE;
    default: return D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
  }
}

D3D12_TEXTURE_ADDRESS_MODE Address(uint32_t clamp) {
  switch (clamp) {
    case 0: return D3D12_TEXTURE_ADDRESS_MODE_WRAP;
    case 1: return D3D12_TEXTURE_ADDRESS_MODE_MIRROR;
    case 2: case 4: return D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
    case 3: case 5: return D3D12_TEXTURE_ADDRESS_MODE_MIRROR_ONCE;
    default: return D3D12_TEXTURE_ADDRESS_MODE_BORDER;
  }
}

// Index list for primitives D3D12 lacks (quad lists, fans), as triangle lists.
bool ExpandPrimitive(uint32_t prim, uint32_t count, std::vector<uint32_t>& order) {
  order.clear();
  if (prim == 13) {  // quad list
    for (uint32_t q = 0; q + 3 < count; q += 4)
      for (uint32_t i : {0u, 1u, 2u, 0u, 2u, 3u}) order.push_back(q + i);
    return true;
  }
  if (prim == 5) {  // triangle fan
    for (uint32_t i = 1; i + 1 < count; ++i)
      for (uint32_t v : {0u, i, i + 1}) order.push_back(v);
    return true;
  }
  return false;
}

}  // namespace

ID3D12PipelineState* NativeRenderer::Pipeline(const Shader& vs, const Shader& ps, uint32_t prim,
                                              RenderTarget* color, RenderTarget* depth) {
  const auto blend = Reg<reg::RB_BLENDCONTROL>(regs_);
  const auto depth_control = Reg<reg::RB_DEPTHCONTROL>(regs_);
  const auto mode = Reg<reg::PA_SU_SC_MODE_CNTL>(regs_);
  const auto stencil = Reg<reg::RB_STENCILREFMASK>(regs_);
  const uint32_t color_mask = regs_[0x2104] & 0xF;
  const bool strip_cut = (prim == 6) && mode.multi_prim_ib_ena;
  const bool clip_disable = Reg<reg::PA_CL_CLIP_CNTL>(regs_).clip_disable;

  uint64_t key = 0xCBF29CE484222325ull;
  auto mix = [&](uint64_t v) { key = (key ^ v) * 0x100000001B3ull; };
  for (char c : vs.name + ps.name) mix(uint8_t(c));
  mix(TopologyType(prim));
  mix(color ? color->format : 0);
  mix(color ? color->samples : depth->samples);
  mix(depth ? depth->format : 0);
  mix(color ? blend.value : 0);
  mix(depth ? depth_control.value : 0);
  mix(depth ? (stencil.value & 0xFFFF00) : 0);
  mix(mode.value & 0x00200007);  // cull, face, multi-prim reset
  mix(color_mask | (strip_cut << 8) | (clip_disable << 9));
  auto it = pipelines_.find(key);
  if (it != pipelines_.end()) return it->second;

  D3D12_GRAPHICS_PIPELINE_STATE_DESC desc = {};
  desc.pRootSignature = root_signature_;
  desc.VS = {vs.dxil.data(), vs.dxil.size()};
  desc.PS = {ps.dxil.data(), ps.dxil.size()};
  auto& rt = desc.BlendState.RenderTarget[0];
  rt.RenderTargetWriteMask = UINT8(color_mask);
  rt.SrcBlend = Blend(blend.color_srcblend);
  rt.DestBlend = Blend(blend.color_destblend);
  rt.BlendOp = BlendOp(blend.color_comb_fcn);
  rt.SrcBlendAlpha = BlendAlpha(blend.alpha_srcblend);
  rt.DestBlendAlpha = BlendAlpha(blend.alpha_destblend);
  rt.BlendOpAlpha = BlendOp(blend.alpha_comb_fcn);
  rt.BlendEnable = !(rt.SrcBlend == D3D12_BLEND_ONE && rt.DestBlend == D3D12_BLEND_ZERO &&
                     rt.BlendOp == D3D12_BLEND_OP_ADD && rt.SrcBlendAlpha == D3D12_BLEND_ONE &&
                     rt.DestBlendAlpha == D3D12_BLEND_ZERO && rt.BlendOpAlpha == D3D12_BLEND_OP_ADD);
  rt.LogicOp = D3D12_LOGIC_OP_NOOP;
  desc.SampleMask = UINT_MAX;
  desc.RasterizerState.FillMode = D3D12_FILL_MODE_SOLID;
  desc.RasterizerState.CullMode = (mode.cull_front && mode.cull_back) ? D3D12_CULL_MODE_NONE
                                  : mode.cull_front ? D3D12_CULL_MODE_FRONT
                                  : mode.cull_back  ? D3D12_CULL_MODE_BACK
                                                    : D3D12_CULL_MODE_NONE;
  desc.RasterizerState.FrontCounterClockwise = !mode.face;  // 0: front is CCW
  desc.RasterizerState.DepthClipEnable = !clip_disable;
  if (depth) {
    auto& ds = desc.DepthStencilState;
    ds.DepthEnable = depth_control.z_enable;
    ds.DepthWriteMask = depth_control.z_write_enable ? D3D12_DEPTH_WRITE_MASK_ALL : D3D12_DEPTH_WRITE_MASK_ZERO;
    ds.DepthFunc = Compare(depth_control.zfunc);
    ds.StencilEnable = depth_control.stencil_enable;
    ds.StencilReadMask = UINT8(stencil.stencilmask);
    ds.StencilWriteMask = UINT8(stencil.stencilwritemask);
    ds.FrontFace = {StencilOp(depth_control.stencilfail), StencilOp(depth_control.stencilzfail),
                    StencilOp(depth_control.stencilzpass), Compare(depth_control.stencilfunc)};
    ds.BackFace = depth_control.backface_enable
                      ? D3D12_DEPTH_STENCILOP_DESC{StencilOp(depth_control.stencilfail_bf),
                                                   StencilOp(depth_control.stencilzfail_bf),
                                                   StencilOp(depth_control.stencilzpass_bf),
                                                   Compare(depth_control.stencilfunc_bf)}
                      : ds.FrontFace;
    desc.DSVFormat = depth->format;
  }
  desc.IBStripCutValue = strip_cut ? D3D12_INDEX_BUFFER_STRIP_CUT_VALUE_0xFFFF
                                   : D3D12_INDEX_BUFFER_STRIP_CUT_VALUE_DISABLED;
  desc.PrimitiveTopologyType = TopologyType(prim);
  desc.NumRenderTargets = color ? 1 : 0;
  if (color) desc.RTVFormats[0] = color->format;
  desc.SampleDesc.Count = color ? color->samples : depth->samples;
  ID3D12PipelineState* pipeline = nullptr;
  if (FAILED(device_->CreateGraphicsPipelineState(&desc, IID_PPV_ARGS(&pipeline)))) {
    REXLOG_ERROR("native_renderer: pipeline failed: {} + {}", vs.name, ps.name);
    pipeline = nullptr;
  }
  pipelines_[key] = pipeline;
  return pipeline;
}

uint32_t NativeRenderer::TextureDescriptor(uint32_t slot, bool* cube) {
  uint32_t fetch[6];
  for (int i = 0; i < 6; ++i) fetch[i] = regs_[0x4800 + 6 * slot + i];
  *cube = false;
  if ((fetch[0] & 3) != 2) return 0;
  TextureCache::Entry* entry = textures_->Get(fetch, physical_);
  if (!entry || !entry->resource) return 0;
  *cube = entry->cube;
  auto key = entry->resource;
  auto it = srv_indices_.find(key);
  if (it != srv_indices_.end() && it->second.second == entry->component_mapping) return it->second.first;
  // One SRV per texture (re-made if its swizzle changes).
  const uint32_t index = view_next_++;
  D3D12_SHADER_RESOURCE_VIEW_DESC srv = {};
  srv.Format = entry->format;
  srv.Shader4ComponentMapping = entry->component_mapping;
  if (entry->cube) {
    srv.ViewDimension = D3D12_SRV_DIMENSION_TEXTURECUBE;
    srv.TextureCube.MipLevels = entry->mip_levels;
  } else {
    srv.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
    srv.Texture2D.MipLevels = entry->mip_levels;
  }
  auto handle = view_heap_->GetCPUDescriptorHandleForHeapStart();
  handle.ptr += size_t(view_increment_) * index;
  device_->CreateShaderResourceView(entry->resource, &srv, handle);
  srv_indices_[key] = {index, entry->component_mapping};
  return index;
}

uint32_t NativeRenderer::SamplerDescriptor(uint32_t slot) {
  const uint32_t* f = &regs_[0x4800 + 6 * slot];
  if ((f[0] & 3) != 2) return 0;
  // Clamp modes, filters, aniso, LOD bias and range, border color.
  const std::vector<uint32_t> key = {(f[0] >> 10) & 0x1FF, (f[3] >> 19) & 0x1FF, f[4] & 0x3FFFFF,
                                     f[5] & 3};
  auto it = sampler_indices_.find(key);
  if (it != sampler_indices_.end()) return it->second;
  D3D12_SAMPLER_DESC desc = {};
  const uint32_t mag = (f[3] >> 19) & 3, min = (f[3] >> 21) & 3, mip = (f[3] >> 23) & 3;
  const uint32_t aniso = (f[3] >> 25) & 7;
  if (aniso >= 2 && aniso <= 5) {
    desc.Filter = D3D12_FILTER_ANISOTROPIC;
    desc.MaxAnisotropy = 1u << (aniso - 1);
  } else {
    desc.Filter = D3D12_ENCODE_BASIC_FILTER(min == 1 ? D3D12_FILTER_TYPE_LINEAR : D3D12_FILTER_TYPE_POINT,
                                            mag == 1 ? D3D12_FILTER_TYPE_LINEAR : D3D12_FILTER_TYPE_POINT,
                                            mip == 1 ? D3D12_FILTER_TYPE_LINEAR : D3D12_FILTER_TYPE_POINT,
                                            D3D12_FILTER_REDUCTION_TYPE_STANDARD);
    desc.MaxAnisotropy = 1;
  }
  desc.AddressU = Address((f[0] >> 10) & 7);
  desc.AddressV = Address((f[0] >> 13) & 7);
  desc.AddressW = Address((f[0] >> 16) & 7);
  desc.MipLODBias = float(int32_t(f[4] << 10) >> 22) / 32.0f;
  desc.ComparisonFunc = D3D12_COMPARISON_FUNC_NEVER;
  // Border: ABGR black, ABGR white, ACBYCR black, ACBCRY black (as the SDK maps them).
  static const float kBorder[4][4] = {{0, 0, 0, 0}, {1, 1, 1, 1}, {0.5f, 0, 0.5f, 0}, {0, 0.5f, 0.5f, 0}};
  std::copy_n(kBorder[f[5] & 3], 4, desc.BorderColor);
  // The texture's own mips bound the LOD above, as in the SDK; base map: no mips.
  desc.MinLOD = float((f[4] >> 2) & 0xF);
  desc.MaxLOD = mip == 2 ? desc.MinLOD : D3D12_FLOAT32_MAX;
  const uint32_t index = sampler_next_++;
  auto handle = sampler_heap_->GetCPUDescriptorHandleForHeapStart();
  handle.ptr += size_t(sampler_increment_) * index;
  device_->CreateSampler(&desc, handle);
  sampler_indices_[key] = index;
  return index;
}

void NativeRenderer::FillSharedConstants(uint8_t* shared) {
  auto put = [&](uint32_t offset, uint32_t value) { std::memcpy(shared + offset, &value, 4); };
  // Per fetch slot: 2D, 3D and cube descriptor indices, then the sampler.
  for (uint32_t slot = 0; slot < 32; ++slot) {
    bool cube = false;
    const uint32_t texture = TextureDescriptor(slot, &cube);
    put(4 * slot, texture && !cube ? texture : kNullTexture2D);
    put(4 * (32 + slot), kNullTexture3D);  // shortcut: no 3D textures yet; add when a shader samples one
    put(4 * (64 + slot), texture && cube ? texture : kNullTextureCube);
    put(4 * (96 + slot), SamplerDescriptor(slot));
  }
  // Bools: XenosRecomp packs VS b0-b15 in bits 0-15 and PS b0-b15 in 16-31.
  put(512, (regs_[0x4900] & 0xFFFF) | ((regs_[0x4904] & 0xFFFF) << 16));
  put(516, 0);  // g_SwappedTexcoords: vertex streams are converted on the CPU
  std::memcpy(shared + 528, &regs_[0x210E], 4);  // RB_ALPHA_REF
}

void NativeRenderer::DoDraw(const Draw& draw, uint8_t* base) {
  ++draws_;
  if (int32_t(frames_) == REXCVAR_GET(trace_native_frame))
    REXLOG_INFO("trace_native: draw prim {} {} start {} n {} base {} inline {:08X}/{} ib {:08X}", draw.prim,
                draw.indexed ? "idx" : "seq", draw.start, draw.count, draw.base_vertex, draw.inline_vertices,
                draw.inline_stride, draw.index_address);
  if (!BeginFrame()) return;
  const auto mode_control = Reg<reg::RB_MODECONTROL>(regs_);
  const uint32_t edram_mode = uint32_t(mode_control.edram_mode);
  if (edram_mode != 4 && edram_mode != 5) return Skip("EDRAM mode " + std::to_string(edram_mode));
  Shader* vs = ShaderFor(bound_vs_, base);
  Shader* ps = ShaderFor(bound_ps_, base);
  // A shader translated on a worker thread (ShaderFor) is used from the first draw after it is done.
  auto ready = [](Shader* shader) {
    if (shader && shader->translating.valid() &&
        shader->translating.wait_for(std::chrono::seconds(0)) == std::future_status::ready) {
      shader->dxil = shader->translating.get();
      shader->loaded = !shader->dxil.empty();
    }
    return shader && shader->loaded;
  };
  if (!ready(vs) || !ready(ps))
    return Skip((vs && vs->translating.valid()) || (ps && ps->translating.valid()) ? "shader still translating"
                                                                                   : "shader not translated");
  if (vs_code_.empty()) return Skip("no vertex shader microcode");

  auto& fetches = fetches_[{vs->name, vs_code_hash_}];
  if (!fetches.valid && fetches.error.empty()) {
    fetches = BuildVertexFetches(vs_code_.data(), vs_code_.size(), vs->elements);
    if (fetches.descriptors.size() > 32) fetches = {{}, {}, false, "more than 32 vertex fetches"};
  }
  if (!fetches.valid) return Skip(fetches.error);

  const auto depth_control = Reg<reg::RB_DEPTHCONTROL>(regs_);
  RenderTarget* color = edram_mode == 4 ? ColorTarget() : nullptr;
  RenderTarget* depth = (depth_control.z_enable || depth_control.stencil_enable) ? DepthTarget() : nullptr;
  if (color && depth && (color->width != depth->width || color->height != depth->height)) depth = nullptr;
  if (!color && !depth) return Skip("no render target");

  // Indices: the guest's (the shaders fetch vertices by index themselves, and
  // see r0.x = SV_VertexID + index_base, the guest's vertex index).
  std::vector<uint32_t> indices;
  int32_t index_base = 0;
  if (draw.indexed) {
    if (!draw.index_address) return Skip("indexed draw without an index buffer");
    indices.resize(draw.count);
    const uint8_t* src = physical_ + draw.index_address + draw.start * (draw.index32 ? 4 : 2);
    for (uint32_t i = 0; i < draw.count; ++i)
      indices[i] = draw.index32 ? rex::memory::load_and_swap<uint32_t>(src + 4 * i)
                                : rex::memory::load_and_swap<uint16_t>(src + 2 * i);
    index_base = draw.base_vertex;  // VGT_INDX_OFFSET
  }
  std::vector<uint32_t> expanded;
  const uint32_t prim = draw.prim;
  if (ExpandPrimitive(prim, draw.indexed ? uint32_t(indices.size()) : draw.count, expanded)) {
    for (auto& v : expanded) v = draw.indexed ? indices[v] : draw.start + v;
    indices = std::move(expanded);
  }
  const uint32_t topology_prim = (prim == 13 || prim == 5) ? 4 : prim;
  if (topology_prim != 1 && topology_prim != 2 && topology_prim != 3 && topology_prim != 4 &&
      topology_prim != 6)
    return Skip("primitive type " + std::to_string(prim));

  ID3D12PipelineState* pipeline = Pipeline(*vs, *ps, topology_prim, color, depth);
  if (!pipeline) return Skip("pipeline creation failed");

  // Vertex streams, raw, and the fetch descriptors with each stream's endian and size.
  // shortcut: a stream range is uploaded once per frame; if a game rewrites one
  // between draws of a frame, key the upload by content.
  D3D12_GPU_VIRTUAL_ADDRESS stream_va[4] = {};
  uint32_t stream_endian[4] = {}, stream_size[4] = {};
  for (uint32_t j = 0; j < fetches.streams.size(); ++j) {
    const uint32_t slot = fetches.streams[j];
    uint32_t address, size;
    if (draw.inline_vertices && slot == 95) {  // Begin*Vertices writes 8in32 fetches
      address = draw.inline_vertices;
      size = draw.inline_size;
      stream_endian[j] = 2;
    } else {
      const uint32_t d0 = regs_[0x4800 + 2 * slot], d1 = regs_[0x4800 + 2 * slot + 1];
      if ((d0 & 3) != 3) return Skip("vertex fetch constant not set");
      address = d0 & ~3u;
      size = ((d1 >> 2) & 0xFFFFFF) * 4;
      stream_endian[j] = d1 & 3;
    }
    if (!size || uint64_t(address) + size > 0x20000000) return Skip("vertex stream outside memory");
    // shortcut: streams are uploaded whole; one this big would mean an upload per draw of the
    // whole thing, so it is skipped until uploads are limited to the range a draw reads.
    if (size > (16u << 20)) return Skip("vertex stream over 16 MB");
    stream_va[j] = StreamAddress(address, size);
    stream_size[j] = size;
  }
  uint8_t fetch_constants[16 + 32 * 16] = {};
  std::memcpy(fetch_constants, &index_base, 4);
  for (size_t k = 0; k < fetches.descriptors.size(); ++k) {
    auto d = fetches.descriptors[k];
    if (d[1] >> 31) {
      const uint32_t j = (d[1] >> 16) & 3;
      d[1] |= stream_endian[j] << 20;
      d[3] = stream_size[j];
    }
    std::memcpy(fetch_constants + 16 + 16 * k, d.data(), 16);
  }

  // Constants.
  const auto vte = Reg<reg::PA_CL_VTE_CNTL>(regs_);
  float xscale = vte.vport_x_scale_ena ? RegFloat(regs_, 0x210F) : 1.0f;
  float xoffset = vte.vport_x_offset_ena ? RegFloat(regs_, 0x2110) : 0.0f;
  float yscale = vte.vport_y_scale_ena ? RegFloat(regs_, 0x2111) : 1.0f;
  float yoffset = vte.vport_y_offset_ena ? RegFloat(regs_, 0x2112) : 0.0f;
  float zscale = vte.vport_z_scale_ena ? RegFloat(regs_, 0x2113) : 1.0f;
  float zoffset = vte.vport_z_offset_ena ? RegFloat(regs_, 0x2114) : 0.0f;
  // The shaders end with oPos.xy = oPos.xy * ndc_scale + ndc_offset * oPos.w
  // (patches/xenos-recomp.diff). Clipped draws use the guest viewport (a
  // positive Y scale, which D3D12 cannot take, flips Y in the shader instead);
  // unclipped ones, usually in pixels with the viewport transform off, get the
  // whole target and the guest transform in the shader (as Xenia does).
  // Inverted Z ranges (reverse Z: scale -1, offset 1) pass through; D3D12 allows Min > Max.
  RenderTarget* size_from = color ? color : depth;
  D3D12_VIEWPORT viewport = {xoffset - std::abs(xscale), yoffset - std::abs(yscale),
                             2.0f * std::abs(xscale), 2.0f * std::abs(yscale),
                             std::clamp(zoffset, 0.0f, 1.0f), std::clamp(zoffset + zscale, 0.0f, 1.0f)};
  float ndc_scale[2] = {1.0f, yscale > 0 ? -1.0f : 1.0f}, ndc_offset[2] = {0.0f, 0.0f};
  if (Reg<reg::PA_CL_CLIP_CNTL>(regs_).clip_disable) {
    viewport.TopLeftX = viewport.TopLeftY = 0.0f;
    viewport.Width = float(size_from->width);
    viewport.Height = float(size_from->height);
    ndc_scale[0] = 2.0f * xscale / viewport.Width;
    ndc_scale[1] = -2.0f * yscale / viewport.Height;
    ndc_offset[0] = 2.0f * xoffset / viewport.Width - 1.0f;
    ndc_offset[1] = 1.0f - 2.0f * yoffset / viewport.Height;
  }
  if (Reg<reg::PA_SU_VTX_CNTL>(regs_).pix_center == xenos::PixelCenter::kD3DZero) {
    // Direct3D 9 pixel centers are at .0: move by half a host pixel.
    ndc_offset[0] += 1.0f / viewport.Width;
    ndc_offset[1] -= 1.0f / viewport.Height;
  }
  uint8_t shared[768] = {};
  FillSharedConstants(shared);
  std::memcpy(shared + 520, ndc_offset, 8);  // g_HalfPixelOffset
  std::memcpy(shared + 536, ndc_scale, 8);   // g_NdcScale
  const auto color_control = Reg<reg::RB_COLORCONTROL>(regs_);
  uint32_t spec = 0;
  if (color_control.alpha_test_enable) {
    const uint32_t func = uint32_t(color_control.alpha_func);
    if (func == 0) return;          // never passes
    if (func == 4 || func == 6) spec |= 2;  // greater (or equal): XenosRecomp clips below the ref
    else if (func != 7) Skip("alpha test function " + std::to_string(func));
  }

  if (int32_t(frames_) == REXCVAR_GET(trace_native_frame)) {
    std::string textures;
    for (uint32_t slot = 0; slot < 32; ++slot) {
      const uint32_t* f = &regs_[0x4800 + 6 * slot];
      uint32_t index;
      std::memcpy(&index, shared + 4 * slot, 4);
      if ((f[0] & 3) == 2)
        textures += " t" + std::to_string(slot) + "=" + std::to_string(f[1] & 0x3F) + ":" +
                    std::to_string((f[2] & 0x1FFF) + 1) + "x" + std::to_string(((f[2] >> 13) & 0x1FFF) + 1) +
                    (index == kNullTexture2D ? "(null)" : "");
    }
    for (const auto& d : fetches.descriptors)
      textures += " f" + std::to_string(d[2] & 0x3F) + "@s" + std::to_string((d[1] >> 16) & 3) + "+" +
                  std::to_string(int32_t(d[0])) + "/" + std::to_string(d[1] & 0xFFFF);
    for (uint32_t slot : fetches.streams) textures += " s" + std::to_string(slot);
    REXLOG_INFO("trace_native: {}+{} prim {} {} n={} rt {}{}@{} vp {} {} {} {} z {}-{} blend {:08X} mask {:X} "
                "depth {:08X} alpha {:X}/{} streams {} clip {:08X} vte {:08X}{}",
                vs->name, ps->name, prim, draw.indexed ? "idx" : "seq", draw.count, color ? "C" : "-",
                depth ? "D" : "-", regs_[0x2001] & 0xFFF, viewport.TopLeftX, viewport.TopLeftY, viewport.Width, viewport.Height,
                RegFloat(regs_, 0x2113), RegFloat(regs_, 0x2114), regs_[0x2201], regs_[0x2104] & 0xF, regs_[0x2200],
                color_control.value & 0xF, std::bit_cast<float>(regs_[0x210E]), fetches.streams.size(),
                regs_[0x2204], regs_[0x2206], textures);
  }
  list_->SetPipelineState(pipeline);
  list_->SetGraphicsRootConstantBufferView(0, Upload(&regs_[0x4000], 4096, 256));
  list_->SetGraphicsRootConstantBufferView(1, Upload(&regs_[0x4400], 4096, 256));
  list_->SetGraphicsRootConstantBufferView(2, Upload(shared, sizeof(shared), 256));
  list_->SetGraphicsRoot32BitConstant(3, spec, 0);
  const auto views_start = view_heap_->GetGPUDescriptorHandleForHeapStart();
  for (UINT i = 4; i < 7; ++i) list_->SetGraphicsRootDescriptorTable(i, views_start);
  list_->SetGraphicsRootDescriptorTable(7, sampler_heap_->GetGPUDescriptorHandleForHeapStart());
  list_->SetGraphicsRootConstantBufferView(8, Upload(fetch_constants, sizeof(fetch_constants), 256));
  for (UINT j = 0; j < 4; ++j)  // unused streams are never read, but must be valid addresses
    list_->SetGraphicsRootShaderResourceView(9 + j, stream_va[j] ? stream_va[j] : Upload(fetch_constants, 16, 256));

  // The screen scissor; while tiled it is the tile, and we draw every tile at once.
  const uint32_t tl = Tiled() ? 0 : regs_[0x200E], br = Tiled() ? 0xFFFFFFFF : regs_[0x200F];
  D3D12_RECT scissor = {LONG(tl & 0x7FFF), LONG((tl >> 16) & 0x7FFF),
                        LONG(std::min(br & 0x7FFF, size_from->width)),
                        LONG(std::min((br >> 16) & 0x7FFF, size_from->height))};
  list_->RSSetViewports(1, &viewport);
  list_->RSSetScissorRects(1, &scissor);
  if (color) Transition(color->resource, color->state, D3D12_RESOURCE_STATE_RENDER_TARGET);
  if (depth) Transition(depth->resource, depth->state, D3D12_RESOURCE_STATE_DEPTH_WRITE);
  list_->OMSetRenderTargets(color ? 1 : 0, color ? &color->view : nullptr, FALSE,
                            depth ? &depth->view : nullptr);
  list_->OMSetStencilRef(regs_[0x210D] & 0xFF);
  const float blend_factor[4] = {RegFloat(regs_, 0x2105), RegFloat(regs_, 0x2106),
                                 RegFloat(regs_, 0x2107), RegFloat(regs_, 0x2108)};
  list_->OMSetBlendFactor(blend_factor);
  static const D3D12_PRIMITIVE_TOPOLOGY kTopology[] = {
      D3D_PRIMITIVE_TOPOLOGY_UNDEFINED,    D3D_PRIMITIVE_TOPOLOGY_POINTLIST,
      D3D_PRIMITIVE_TOPOLOGY_LINELIST,     D3D_PRIMITIVE_TOPOLOGY_LINESTRIP,
      D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST, D3D_PRIMITIVE_TOPOLOGY_UNDEFINED,
      D3D_PRIMITIVE_TOPOLOGY_TRIANGLESTRIP};
  list_->IASetPrimitiveTopology(kTopology[topology_prim]);
  if (!indices.empty()) {
    // shortcut: strip restart is 0xFFFF (16-bit guest indices); 32-bit ones would need 0xFFFFFFFF.
    const bool narrow = draw.indexed && !draw.index32;
    std::vector<uint16_t> narrow_indices;
    D3D12_INDEX_BUFFER_VIEW ibv = {};
    if (narrow) {
      narrow_indices.assign(indices.begin(), indices.end());
      ibv = {Upload(narrow_indices.data(), narrow_indices.size() * 2, 16),
             UINT(narrow_indices.size() * 2), DXGI_FORMAT_R16_UINT};
    } else {
      ibv = {Upload(indices.data(), indices.size() * 4, 16), UINT(indices.size() * 4),
             DXGI_FORMAT_R32_UINT};
    }
    list_->IASetIndexBuffer(&ibv);
    list_->DrawIndexedInstanced(UINT(indices.size()), 1, 0, 0, 0);
  } else {
    list_->DrawInstanced(draw.count, 1, draw.start, 0);
  }
}

}  // namespace blitz
