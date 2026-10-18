# XDK D3D device map (native renderer, phase N2)

How Blitz's statically linked XDK D3D turns the game's draw calls into GPU
state, measured against the register file Xenia decodes for the same draws.
The native renderer reads state the same way. Phase plan:
`docs/native-renderer-plan.md`.

## The device and its command buffer

The game passes the D3D device in `r3` to every draw call. Its address is a
heap pointer (0x4042CE80 in these runs), so it is read at runtime, never
hardcoded.

| Device field | Meaning |
|---|---|
| `+0` (u64) | Dirty mask, VS float constants: bit *i* (from the top) = constants 4i..4i+3 |
| `+8` (u64) | Dirty mask, PS float constants |
| `+16`, `+24`, `+32` (u64) | Dirty masks for the register blocks, fetch constants (`+24` low word), clip planes and bool constants |
| `+48` | Command buffer: address of the last dword written (writes use `stwu 4(rX)`) |
| `+56` | Command buffer limit; past it, `sub_8251F770` kicks the buffer off |
| `+1152` … `+10864` | Register shadows (next table) |
| `+12812` | Bound index buffer object: `+0` flags (bit 31 = 32-bit indices), `+24` address |
| `+13844` | End of the inline vertex block reserved by `Begin*Vertices` |

## Entry points

Named in `config/functions.toml`. The argument lists match the XDK API
functions of the same names, and every draw packet field was checked against
them (Verification).

| Address | Name | Arguments after `dev` |
|---|---|---|
| `0x8251B220` | `D3DDevice_DrawVertices` | prim, start, count |
| `0x8251B638` | `D3DDevice_DrawIndexedVertices` | prim, base vertex, start index, index count |
| `0x8251A7D8` | `D3DDevice_BeginVertices` | prim, count, stride; returns inline vertex memory |
| `0x8251ACE0` | `D3DDevice_BeginIndexedVertices` | prim, base vertex, vertex count, index count, … |
| `0x8251A7C8` | `D3DDevice_EndVertices` | moves `+48` past the inline data |
| `0x8251AC98` | `D3DDevice_DrawVerticesUP` | prim, count, data, stride (Begin + memcpy + End) |
| `0x82521420` | `D3D_FlushRegisters` | mask, first register, shadow: 1 register per bit |
| `0x825217B8` | `D3D_FlushFloatConstants` | mask, first register, shadow: 16 registers per bit |
| `0x82521660` | `D3D_FlushFetchConstants` | mask: 6 registers per bit from `+1152` |
| `0x825214E0` | `D3D_FlushClipPlanes` | mask: 4 registers per bit from `+10272` |

Also known, not renamed: present `sub_8251D580` (the only `VdSwap` caller),
fence wait `sub_8251E390`, kick-off `sub_8251F770`, GPU interrupt callback
`0x8251E0C8`.

## Register shadows

Every draw call starts by flushing the dirty parts of these blocks through the
four flush functions, as type-0 packets, then writes its draw packet.

| Device offset | Registers | Contents |
|---|---|---|
| `+1152` | `0x4800`–`0x48BF` | Fetch constants, 32 slots × 6 dwords |
| `+1920` | `0x4000`–`0x43FF` | VS float constants c0–c255 |
| `+6016` | `0x4400`–`0x47FF` | PS float constants c0–c255 |
| `+10112` | `0x4900`–`0x4907` | Bool constants |
| `+10272` | `0x2388`–`0x239F` | User clip planes `PA_CL_UCP_0..5` |
| `+10368` | `0x2000`–`0x200F` | `RB_SURFACE_INFO` … `PA_SC_SCREEN_SCISSOR_BR` |
| `+10444` | `0x2100`–`0x2114` | `VGT_MAX_VTX_INDX` … viewport (`PA_CL_VPORT_*`) |
| `+10528` | `0x2180`–`0x2184` | `SQ_PROGRAM_CNTL` … `SQ_WRAPPING_1` |
| `+10548` | `0x2200`–`0x220B` | `RB_DEPTHCONTROL` … `RB_BLENDCONTROL3` |
| `+10596` | `0x2280`–`0x2294` | `PA_SU_POINT_SIZE` … `VGT_ENHANCE` |
| `+10680` | `0x2300`–`0x2325` | `PA_SC_LINE_CNTL` … `RB_SAMPLE_COUNT_ADDR` |
| `+10832` | `0x2380`–`0x2387` | Polygon offset, `PA_CL_POINT_CULL_RAD` |

**The GPU sees the shadow as flushed, not the shadow as it is.** The game
sometimes changes a shadow entry without marking it dirty. Measured example:
the type bits of fetch slots 16–19 (the k_32_32_32_32 data textures) were
cleared in the shadow 13 draws before a flush sent them. Reading the shadow at
draw time is wrong for 0.004% of values. Replaying the flushes is exact.

## Registers written outside the shadow

| Registers | Source |
|---|---|
| Float constants at the top of the range (VS and PS c252–c255 in this run) | The shader's literal constants, written when a shader is bound. N3 takes them from the shader itself. |
| Fetch slot 31 (`0x48BA`–`0x48BF`) on inline-vertex draws | `Begin*Vertices` points it at the vertex space it reserves in the command buffer |
| `VGT_DRAW_INITIATOR`, `VGT_DMA_BASE`, `VGT_DMA_SIZE`, `VGT_INDX_OFFSET` | The draw call's arguments and the bound index buffer |
| `PA_SC_WINDOW_OFFSET`, `PA_SC_WINDOW_SCISSOR_TL/BR` | Predicated tiling. Some passes render a 1280×720 target as two 640-wide tiles. A native renderer draws the whole target once. |

## Draws that are not game draw calls

- Clear/resolve helpers (`sub_8250C938`, `sub_82519F50`, `sub_8251BD40`,
  about 17 draws per frame). Their rect-list draws carry their own state; N4
  implements clears and resolves as operations, not as draws.
- A fixed D3D-internal sequence at physical `0x1FC94980`: 24 point-list
  packets, replayed 5 times per frame through an indirect buffer, with inline
  shaders. None of the 3,360 sampled draws writes color, depth or stencil
  (`RB_COLOR_MASK` 0, depth/stencil disabled). The native renderer skips them.

## Resolves, clears and tiling

Named in `config/functions.toml`, decoded in `src/renderer/d3d_ops.cpp`. The
argument order was read from the asm because this XDK build differs from the
documented one. Arguments past r10 are in 8-byte stack slots at r1+20+8n.

| Call | Arguments |
|---|---|
| `D3DDevice_Resolve` (`0x8250C938`) | r4 flags (bits 0–2 source: 0–3 render target, 4 depth; 0x100 clear color; 0x200 clear depth/stencil), r5 source rect (null: destination size), r6/r7 unknown (always 0 on the tape), r8 destination texture, r9 destination point, r10 level, slot 8 slice, slot 9 clear color (float4), f1 clear Z, slot 11 stencil, slot 12 parameters |
| `D3DDevice_ClearF` (`0x82519F50`) | r4 flags, r5 rect, r6 color (float4), f1 Z, r8 stencil |
| `D3DDevice_BeginTiling` (`0x8251BD40`) | r4 flags, r5 tile count (stored at device+13124), r6 tile rects, then clear values |

- Texture objects hold their fetch constant at +28 with *virtual* addresses;
  D3D sends the GPU physical ones (`(a & 0x1FFFFFFF)`, +4 KB for 0xE0000000+).
- Render target surfaces are bound at device+12816…12828, depth at +12832.
- Some passes render a 1280×720 target as two 640-wide tiles with 2× or 4×
  MSAA and resolve once per tile. The second tile's copy lands at the tile's
  origin (minus `PA_SC_WINDOW_OFFSET`). A renderer without tiling copies each
  tile's rectangle.
- Checked by `tools/ops_check.py` against Xenia's copy registers: 44 resolves
  out of 44 match in source, clear flags, destination address (plus tile
  origin), format and size. Depth resolves copy the raw 24:8 words as 32 bpp.
  Where ClearF's color reaches the GPU is not decoded yet.

## For the native renderer

Hook the four flush functions and apply `(mask, first register, shadow)` to a
native register file. That reproduces exactly what the GPU receives, without
parsing PM4. Add the draw calls (packet fields from their arguments),
`Begin*Vertices` (slot 31), and the shader binds (N3).

## Verification

Tools: SDK patch 6 (`--trace_draws`) logs Xenia's registers per draw.
`src/renderer/device_trace.cpp` (`--trace_device`) logs every D3D call that
writes commands, with device snapshots. `tools/device_map.py <cp.bin> <dev.bin>`
pairs each draw with the call that wrote its packet (by address) and checks:

```
--trace_draws=cp.bin --trace_draws_frames=F --trace_device=dev.bin --trace_device_frames=F
```

Result on `tests/input/one_week_mash.tape`, frames 300/301, 1300/1301, …,
13300/13301 (28 frames, title to results screen):

- 18,060 draws: 14,212 game draws, 488 clear/resolve helper draws, and 3,360
  internal helper draws.
- 34,009,316 shadow register values on game draws. Flushed shadow == Xenia for
  33,866,470. The rest: 137,568 explained as shader literals and 5,278 as
  inline vertex slot 31. **0 unexplained.**
- Draw packet vs call arguments: prim and count on all 14,212, index offset on
  6,190, DMA base and size on 5,296. **0 mismatches.**

Not verified yet:
- The literal rule is a consistency check: the value has been unchanged since
  the last shader bind. N3 replaces it with the literals parsed from each shader.
- No device field holds the shader microcode address. Bound shaders are
  pointers to shader objects, which N3 maps through the bind calls.
- 28 sampled frames, not every frame of the tape.
- The clear/resolve helpers' state (N4).

## Learned while drawing natively (N5)

- `D3DDevice_BeginIndexedVertices` takes (dev, prim, base vertex, vertex
  count, index count, index format, stride, &indices, &vertices); the 9th
  argument is on the stack at r1+84. Index format bit 2 means 32-bit indices.
  It returns an HRESULT, reserves vertex and index space in the command
  buffer, points fetch constant 95 at the vertices (8in32), writes
  `VGT_INDX_OFFSET` = base vertex, and calls `D3D_FlushShaders` when the
  stride changes.
- `D3DDevice_DrawVerticesUP` is `BeginVertices`, a copy, and its own end: it
  never calls `EndVertices`.
- `D3D_FlushShaders` uploads vertex shaders with `IM_LOAD` (from memory) or
  `IM_LOAD_IMMEDIATE` (the code inside the packet, used when an inline draw
  changes the stride).
- When D3D patches a vertex shader for a declaration it also reorders the
  fetches by offset: later ones become mini fetches, and destination registers
  and swizzles move with them (16-bit pairs get `yx`, D3DCOLOR gets `zyxw`).
  Match fetches by destination register, not by slot.
- Device `+56` is a soft limit: writes go past it, then `sub_8251F770` (dev)
  kicks the segment off and returns the new write pointer. A call can span
  two segments.
- The viewport maps Z with scale −1, offset 1 (reverse Z); depth clears are 0.
- `D3DDevice_Resolve` writes the copy registers (RB_COPY_CONTROL..DEST_INFO)
  before its copy draw and resets RB_COPY_CONTROL and RB_COPY_DEST_INFO to 0
  after it, so read the first write. Blitz's color resolves all set
  copy_dest_swap (red and blue swapped) with 8in32 endian; their textures
  sample with `zyxw`, which swaps back. The frontbuffer resolve uses endian 0.
- Song time follows the audio clock; the city and band also follow the CPU
  clock. Two runs only repeat frame for frame with both tied to frames (SDK
  patches 9 and 10).
