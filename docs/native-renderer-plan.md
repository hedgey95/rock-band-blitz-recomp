# Native D3D12 renderer plan

Goal: replace the SDK's emulated GPU (Xenia's PM4 command processor, EDRAM
emulation and runtime shader translator, loaded as `--gpu_plugin=xenos`) with a
renderer that handles Blitz's draws directly on D3D12, the way UnleashedRecomp
does. The emulated GPU stays as a development reference until the native path
reaches parity. It is never shipped.

## What the game asks of the GPU (measured 2026-10-17)

Measured with a temporary census in the stock command processor while replaying
`tests/input/one_week_mash.tape` at a steady 60 fps:

| | Menus | One Week gameplay |
|---|---|---|
| Draws per frame | 65–280 | 770–850 |
| Indexed draws per frame | 8–150 | 240–280 |
| Triangle list / strip / quad / rect draws per frame | | ~650 / 15 / 5 / 3 |
| Point draws without a pixel shader (queries or setup) | 54–120 | 96–120 |
| Resolves (EDRAM → texture copies) per frame | 2–5 | 17 |
| Memexport draws | 0 | 0 |
| Unique vertex / pixel shaders / pairs (cumulative) | 10 / 11 / 16 | 41 / 72 / 113 |

Also measured:
- `k_32_32_32_32` UNORM data textures drive the highway and the 3D models. This
  is SDK patch 5; the native path must keep it.
- Shaders come precompiled from the archive's `gen/` cache (`shaders/%s.fx` →
  `%s/gen/%s_%llx_%s.%s_%s`), with no runtime HLSL. The bounded set is
  enumerable with `--dump_shaders`.
- 67 functions in the binary build PM4 packets, clustered around
  0x8250xxxx–0x8252xxxx. Among them: 4 auto-index draw builders
  (`sub_8250E1A0`, `sub_8250E2F8`, `sub_82515D98`, `sub_82527410`), the present
  (`sub_8251D580`, the only VdSwap caller), the fence wait (`sub_8251E390`),
  the kick-off (`sub_8251F770`) and the GPU interrupt callback (`0x8251E0C8`).
  Some emit predicated-tiling packets (SET_BIN_MASK/SELECT), which a native
  renderer does not need.
- RTTI names 758 classes, including Harmonix's Xbox back end: DxRnd, DxMesh,
  DxMat, DxTex, DxShader, DxShaderMgr, DxTexRenderer, DxMovie, DxParticleSys,
  DxRndOcclusionQueryMgr and the portable Rnd* layer above it.

## Where to hook: the XDK D3D layer (recommended) or Harmonix's Dx* layer

**XDK D3D layer (recommended):** replace the statically linked D3D functions
(resource creation and locks, shader creation, Draw*, Resolve, Clear, Present,
fences). At each draw, read state from the D3D device's shadow copy, because
release XDK builds inline many Set* calls into plain field writes plus dirty
bits. This is UnleashedRecomp's approach.
- It has an oracle: while the emulated GPU still runs, every native draw can be
  checked against the register state Xenia decodes for the same draw. The
  device layout can also be derived automatically by correlating device fields
  with the registers the flush code writes.
- Its risk is the inlined state writes. They are handled by reading the device
  struct, never by hooking each setter.

**Harmonix Dx* layer (fallback):** fewer and higher-level entry points
(DxMesh::Draw, DxMat::Select, DxTex creation), found through the RTTI vtables.
Each one, though, depends on engine data structures we would have to
reverse-engineer, and nothing checks the result draw by draw. Use it only for
a subsystem where the D3D layer turns out to be awkward (movies, perhaps).

## Phases and gates

Each phase ends with a measured gate, using input tapes so runs are repeatable.

| Phase | Work | Gate |
|---|---|---|
| N1 Blind run | Our own `IGraphicsSystem` with no GPU plugin: a D3D12 device and swap chain, a present hook, and the game's GPU sync kept alive (ring read-pointer write-back, fences, vblank interrupt) without executing PM4. | One Week plays to the FINISH screen with audio on a black screen, at 60 fps, with the same score as the reference tape run. |
| N2 Device map | Name the D3D entry points and derive the device shadow-state layout by correlating with the decoded PM4 registers. Record the names in `config/functions.toml`. | A per-draw state dump from device fields matches Xenia's decoded registers for 100% of draws on the tape. |
| N3 Shaders | Capture shader containers at creation and translate them with XenosRecomp (MIT; adapted for Blitz) to HLSL, then DXIL with DXC, cached offline plus at runtime. | All captured shaders translate and compile; the count matches `--dump_shaders`. |
| N4 Resources | Textures (untile, convert formats, including the 32-bit UNORM loader), vertex and index buffers (endian swap), render targets as native D3D12 resources, resolves as copies. | A debug viewer shows the game's textures correctly; resolve outputs match the reference by pixel diff. |
| N5 Draws | PSO cache from device state, constant buffers, the draw path, clears, present. Menus first, then gameplay. | Screenshots at fixed tape frames: title, menus, song select and One Week gameplay differ from the reference by less than an agreed threshold. |
| N6 Parity | Occlusion queries, post-processing chain, movies, particles; delete the PM4 path from shipping builds. | The full One Week tape renders natively; no GPU plugin in the release build. |

Milestone 4 (frame times) follows N6 and is measured on the native path.

## N1 result (2026-10-17)

`--native_renderer=true` runs `src/renderer/native_graphics.cpp` instead of the
GPU plugin: MMIO registers, a 60 Hz vblank interrupt, and a sync-only PM4 walk
that applies only the side effects the CPU waits on (fence writes, scratch
write-back, interrupts, WAIT_REG_MEM, occlusion-query fakes, read-pointer
write-back). It presents a black screen through the SDK's D3D12 presenter.
Measured on `one_week_mash.tape`:
- The game read all 3,115 tape steps at the same frames as the tape
  (`--input_record` output identical).
- Swap rate: 59.99–60.01 fps for 23 of 25 ten-second windows. The other two:
  42.6 fps during boot and 57.7 fps during song load.
- Score: guest memory dumped at frame 14,000 (`--dump_memory`) on the native
  and the stock path. The heap layout is identical, and every word holding
  3,950, 3,450 or 3,960 sits at the same address in both. The stock run's
  results screen read 3,950 (3,450 base + 500 finale) at that point.
- Not checked: whether the earlier 3,960 (FINISH HUD) vs 3,950 (results
  screen) is a HUD/results difference or run-to-run variance in the reference.

## Code layout

- `src/renderer/` in this project: the native renderer. It is the project's
  own code, not an SDK patch. Entry point: `RuntimeConfig.graphics` set in
  `OnPreSetup` (no SDK change needed).
- `thirdparty/XenosRecomp` (submodule pinned to a commit) plus our
  modifications, kept as a small patch the way the SDK's are.
- The stock GPU stays reachable with `--gpu_plugin=xenos` until N6, as the
  reference renderer for diffs.

## Size and risk

This is the largest part of the project. The renderer is about the size of
UnleashedRecomp's `video.cpp` plus shader work, spread over many sessions.
- **Biggest unknown:** how much D3D state the game writes inline (N2). The
  automatic correlation against Xenia's register state is what keeps that from
  becoming guesswork.
- **Fallback if a phase stalls:** the stock GPU keeps the game playable.

## N2 result (2026-10-17)

Details in `docs/d3d-device-map.md`. The D3D draw entry points and the four
dirty-state flush functions are named in `config/functions.toml`. The device
keeps literal register shadows. The GPU sees them *as flushed*, so the native
renderer hooks the flush functions rather than reading the device at draw time.
`tools/device_map.py` on 28 frames sampled across the tape: every register of
all 14,212 game draws (34.0 M values) matches Xenia. The values come from the
flushed shadow, plus three documented sources outside it: shader literals,
inline-vertex fetch slot 31, and the draw call's own arguments (0 mismatches on
prim, count, index offset, DMA base and size). Tiling registers are ignored
natively. Left for later phases: literals from the parsed shaders (N3), and
the clear/resolve helper draws (N4).

## N3 result (2026-10-17)

- **Where shaders come from.** Harmonix's loader (`sub_823EC540`) reads
  ready-made D3D shader objects (the container's virtual part embedded at
  +872 for vertex shaders, +40 for pixel shaders) and a separate microcode blob.
  It registers them with `D3D_RegisterVertexShader` and
  `D3D_RegisterPixelShader`: about 4,000 shaders at load, the whole library.
  The containers have no constant table; literals sit in c252–c255 of both
  stages (the registers N2 attributed to literals).
- **Capture** (`--capture_shaders=<dir>`, `src/renderer/shader_capture.cpp`):
  each registered object at its first bind for a draw, before
  `D3D_FlushShaders` runs. After that, D3D patches the vertex fetches in place
  for the vertex declaration (layout, and 16-bit texcoord swizzles XenosRecomp
  already handles). The microcode must be read through the physical address
  D3D gives the GPU (+4 KB for 0xE0000000+ addresses); the plain virtual
  address gives other bytes.
- **Translation:** XenosRecomp pinned at `990d03b` plus `patches/xenos-recomp.diff`
  (synthesized constant table, 256 PS constants, 32 fetch slots, `SampleLevel`
  in vertex shaders), built by `tools/xenos_recomp` with the Windows SDK's DXC.
  `tools/translate_shaders.py` runs XenosRecomp and `dxc.exe` per shader.
- **Gate, measured on `one_week_mash.tape`:** 120 shaders bound by draws (42
  VS, 78 PS), all captured, all translated and compiled to DXIL (105 as
  libraries because they use specialization constants). Xenia executed 127
  programs in the same run (`--dump_shaders`, `--store_shaders=false`). Of those,
  78 are captured pixel shaders exactly; 46 are captured vertex shaders with
  only their vertex fetches patched (42 shaders, some under two vertex
  declarations); 3 are D3D built-ins found verbatim in the executable (its
  clear/resolve shaders, replaced in N4). 0 unexplained, and every captured
  shader was executed.
- **Not done yet:** shaders that only other songs, venues or modes bind
  (capture more tapes); the runtime shader cache in the exe (N5); whether the
  HLSL renders correctly (N5 compares screenshots).

## N4 result (2026-10-17)

- **Hooks:** `src/renderer/d3d_hooks.cpp` now owns the single hook of each
  D3D function and forwards to listeners, so the traces, the captures and the
  renderer can all see the same calls. The N2 gate still passes after this
  change.
- **Inventory** (`tools/resource_inventory.py`, 28 tape frames): 429
  textures, all 2D or cube (DXT1, DXT4_5, DXN, 8888, k_32_32_32_32,
  k_24_8_FLOAT); 8in32 vertex streams; 8888 render targets with D24FS8 depth;
  about 14 resolves per frame.
- **Textures:** `src/renderer/texture_decode.cpp` handles untiling, packed
  mips, byte swap, and the 32-bit UNORM to float conversion. It uses the SDK's
  Xenia-derived layout math, compiled in. Checked three ways:
  - Contact sheets (`tools/texture_sheet.py`) show the game's art correctly.
  - All 1,631 mip levels of 4×4 or more, in 450 textures, match a scaled-down
    level 0 (`--check-mips`).
  - In native mode, `src/renderer/texture_cache.cpp` uploads every bound
    texture to D3D12. With `--check_texture_upload`, 2,065 subresources were
    read back from the GPU with 0 differences, at 60 fps.
- **Resolves and clears:** decoded from the D3D calls (`src/renderer/d3d_ops.cpp`).
  44/44 resolves match Xenia's copy registers (`tools/ops_check.py`). Details
  are in `docs/d3d-device-map.md`.
- **Moved to N5:** vertex/index buffers and render targets as D3D12
  resources, executing resolves and clears, and the resolve pixel diff. They
  can only be checked once draws put content in render targets. Also: change
  tracking for textures the game rewrites (the data textures), the 8888
  channel order (no colorful 8888 texture outside resolve targets), and
  ClearF's color path.
- **Test runs no longer show a window** (SDK patch 7, `--window_hidden`):
  screenshots come from the presenter (`--screenshot_at`).

## N5 result (2026-10-18): draws on D3D12; gate passed

`--native_renderer=true --native_shader_dir=<rbb-gamedata>/shader-hlsl` now
draws the whole game on D3D12 (`src/renderer/renderer*.cpp`): title, menus,
song select and One Week gameplay with the city, band, highway and notes.
In a 7,200-frame run of the mash tape it skipped 4 draws (boot frames before
the first resolve, and one shader flush it could not parse). The run stayed at
about 60 fps in gameplay after a dip while the song loads.

What it took, besides the plan above:
- **Vertex fetch in the shader** (patches/xenos-recomp.diff,
  `src/renderer/vertex_input.cpp`). Blitz's 3D shaders compute fetch indices:
  instancing from the vertex index, and an index read from one stream used to
  fetch the others. An input assembler cannot do that, so the guest streams
  are uploaded raw and each fetch decodes itself. D3D reorders a shader's
  fetches by offset when it patches them, moving destination registers and
  swizzles, so patched fetches are matched to the translated ones by register.
  A self-check covers this (`blitz_tests`).
- **Reverse Z**: the game's viewport maps Z with scale −1 and offset 1; D3D12
  takes the inverted depth range as is.
- **Screen-space and flipped draws**: the shaders scale positions into NDC
  (`g_NdcScale`) when the viewport transform is off or Y is flipped.
- **Predicated tiling**: drawn once at full size; tiled clears cover the whole
  target, and the game's per-tile resolves already use screen rects.
- **Inline draws**: `BeginIndexedVertices` (9th argument on the stack), and
  `DrawVerticesUP`, which ends its own inline draw without `EndVertices`.
- **Microcode loads**: `IM_LOAD_IMMEDIATE` as well as `IM_LOAD`; a flush that
  kicks the command buffer off (`sub_8251F770`, now hooked) is read as two
  ranges. Walking past a segment end read garbage and crashed the game.
- **Depth resolves**: a compute copy of the depth plane into an R32_FLOAT
  texture (D3D12 copies cannot take part of a depth-stencil resource). This
  removed a purple haze and washed-out colors in gameplay: a post effect
  samples depth.
- **Gamma ramp**: the DC_LUT writes (MMIO) feed the present pass, as the
  SDK's command processor does.

- **Resolves swap red and blue.** Every color resolve sets RB_COPY_DEST_INFO
  copy_dest_swap, and the textures sample with a `zyxw` swizzle that swaps
  back. The native copy does not swap, so the view does it (read from the
  packets the call wrote; D3D sets the register before its copy and resets it
  to 0 after). Without it, passes that resolve twice cancelled out and the
  depth-of-field composite, which mixes both, turned the sky purple.
- **Cube maps** (patches/xenos-recomp.diff): the direction is `(z, w, x)` of the
  swizzled operand; every reflection was wrong before.
- **Samplers as the SDK builds them** (border colors, the texture's own mip
  range) and **channel replication** for formats with fewer than four channels
  (depth reads as RRRR, DXN as RGGG). **MSAA** where the game asks for it (2×
  in the title's tiled pass; gameplay is 1×), with averaging resolves and
  sample-0 depth resolves.

**How the gate is measured.** `scripts/gate-n5.ps1` runs the One Week mash tape
on the stock renderer and on the native one and compares frames with
`tools/compare_frames.py` ("bad" pixels differ by more than 24/255 in some
channel; frame-exact captures on both sides, SDK patch 8). Gameplay only
repeats frame for frame with the game's clocks tied to frames: audio (SDK
patch 9) and CPU time (patch 10); without them even two stock runs differ in
37–39% of pixels. The reference runs with `--anisotropic_override=-1`, so it
renders what the game asks for (the SDK forces 4× anisotropy by default).
Gameplay frames are counted from the first 3D frame, since async loading moves
the song start by a frame or two; so are the tape's song inputs
(`--input_replay_busy_frame`), or the hit sparks drift against the notes.

| Frames (mash tape) | Native vs stock |
|---|---|
| 1300, 1500, 1750, 2050 (title, offline prompt) | MAE 0.35, 0.00% bad |
| 2400, 2550 (song select) | MAE 0.01, 0.00% bad |
| 2800 (song info) | MAE 0.04, 0.09% bad |
| One Week, 5 s / 20 s / 40 s / 80 s in | MAE 0.08–0.56, 0.03–0.72% bad |

All 11 frames are within the agreed 1%. The gameplay remainder is the hit
sparks, which also differ between two stock runs (0–0.9%).

**Finding differences pass by pass.** `--dump_resolves=<prefix>` (native) reads
back every resolve of the captured frames; a stock run with
`--readback_resolve=full --dump_memory=<prefix>` dumps memory on the GPU
timeline (patch 8); `tools/compare_resolves.py` compares them per destination.
That located the cube and swap bugs in minutes after screenshots alone could
not.

Open after N5:
- 3D textures (none seen), 32-bit strip restart, the Xenos MSAA sample
  positions (D3D12's standard ones are used).
- Uploads: streams go up whole, once per frame each, about 15 MB per frame.
- First-time costs (pipelines, texture decode) make loading run at 17–38 fps;
  the song itself holds 60.

## Runtime shader translation (2026-10-18)

A shader missing from `--native_shader_dir` no longer leaves its draws out for
the whole run (the blue screen with only the HUD at section endings). At its
first bind, `NativeRenderer::ShaderFor` (`src/renderer/renderer.cpp`) starts a
worker thread that does what `tools/translate_shaders.py` does for one shader:
it saves the container as `<name>.xsc` in the shader dir, runs XenosRecomp,
then `dxc` (`vs_6_0`/`ps_6_0`, then `lib_6_3`), and saves `<name>.dxil`
(written as `.dxil.tmp` and renamed, so an interrupted compile leaves no bad
file). Draws that use the shader are skipped as "shader still translating"
until it is ready; then the first draw after that picks it up. Tools run with
no console window, below normal priority, at most a quarter of the cores at a
time. Log lines: `no translated shader <name>; translating it`, then
`translated shader <name> (<target>, <s> s)` or `could not translate ...`
(dxc's messages stay in `<name>.dxc.log`).

The tool paths are compiled in from the build machine (CMakeLists.txt:
XenosRecomp from `out/build/xenos_recomp`, `shader_common.h` from the
submodule, `dxc` from the Windows SDK), so only dev builds can translate. A
release needs those tools shipped with the exe.

Measured on the One Week mash tape (real time, no frame lock):
- **Regeneration.** 7 shaders (2 VS, 5 PS; title and gameplay) deleted from
  a copy of the shader dir (`rbb-gamedata/shader-hlsl-rt-test`). The run
  translated all 7 in 0.1–0.4 s each, skipping 53 draws while they compiled
  (2 at the title, 37 at the song start, 14 later), with no other new skips.
  The regenerated `.hlsl` and `.dxil` are byte-identical to the offline set.
  A second run on that dir translated nothing and skipped only the 3 boot
  draws, as the full set does.
- **Frame times** (present to present, now in the 600-frame log line), two
  runs each with the full shader set: mean after boot 16.78/16.79 ms before,
  16.79/16.79 ms after; frames over 25 ms 240/242 before, 249/250 after (the
  new exe ranged 242–257 over four runs, so the gap is within run-to-run
  variance). Translating 7 shaders mid-run added no slow frames to its windows.
- **N5 gate.** `gate-n5.ps1` failed twice after this change, both times at
  frame +1200 with 1.04% bad pixels, and both times the two runs found their
  first 3D frame 2 frames apart (stock 2883 / native 2885, then 2885 / 2883).
  Paired by equal first 3D frames, the same captures pass 11/11 (worst 0.63%),
  and the new native frames match the pre-change native frames (gate6, 13:04)
  within 0.03%. So the renderer is unchanged; the gate's alignment is flaky
  when the start frames differ (something on screen is not counted from the
  first 3D frame). That something was the tape: its song presses were keyed
  to frames since boot, so a start 2 frames later moved every hit spark 2
  frames against the notes (two stock runs 2 frames apart differ the same
  way, 1.02% at +1200). Fixed by `--input_replay_busy_frame=2883` (the tape's
  first 3D frame): from the run's first 3D frame on, the tape plays relative
  to it (`RebaseInputReplay`, `src/input_replay.cpp`). Three gates in a row
  then passed with starts 0–1 frames apart (2881/2882, 2882/2882,
  2881/2882), gameplay worst 0.59% (mostly 0.02%). The leftover is spark
  jitter that two stock runs with the same start also show (0.58%): the
  game's joypad thread polls between presents, so a press can land a frame
  early or late.
