# SDK patches

`rexglue-sdk.diff` is applied on top of ReXGlue SDK tag `v0.10.0` (commit `f5337cd`):

```bash
git clone --branch v0.10.0 --recursive https://github.com/rexglue/rexglue-sdk.git sdk
git -C sdk apply ../patches/rexglue-sdk.diff
```

Regenerate the file after changing the SDK: `git -C sdk diff > patches/rexglue-sdk.diff`.

## 1. libmspack sources on Windows (`thirdparty/CMakeLists.txt`)

`libmspack/cabextract/mspack/*` is made of git symlinks. Git for Windows checks
them out as one-line text files (`core.symlinks=false`), so the SDK build fails
in `lzxd.c`. The fix points `MSPACK_DIR` at the real sources in
`libmspack/libmspack/mspack`.

## 2. Gap fill misses tiny functions (`src/codegen/phase_gapfill.cpp`)

Gap fill splits uncovered code into functions only at `blr` and at tail calls to
functions it already knows about. This had two effects in Blitz:

- A thunk ending in an indirect tail call (`bctr`) swallowed the function after it.
- In a run of thunks that tail-call each other, only the first thunk was kept.

The dropped functions are reached only through vtables, so the game crashed at
runtime with "Call to invalid or unregistered function" (`0x82789360`,
`0x8279A888`). The fix treats an unconditional `bctr` as a split point. It then
reruns gap fill until a pass adds no function that survives, capped at 16 passes,
with a warning if the cap is hit.

Measured on Blitz: 38,082 → 38,176 functions, converging in 3 passes. Validation
reports no errors, and the generated code has no unresolved-branch traps.
Codegen time went from 15 s to 23 s, and the output is identical across runs.

A broader version that treated every uncovered instruction as a gap was tried
and reverted. It created bogus 4-byte functions on block-ending branches and led
to a new runtime crash at `0x823DD70C`.

## 3. Hybrid-GPU laptops get the integrated GPU (`src/ui/d3d12/d3d12_provider.cpp`)

With `d3d12_adapter=-1` the SDK took the first adapter from `EnumAdapters1`,
which is the Intel iGPU on an Optimus laptop. It now enumerates with
`IDXGIFactory6::EnumAdapterByGpuPreference(HIGH_PERFORMANCE)` when available.

Measured: with no flags the adapter went from Intel Iris Xe to the RTX 3080 Ti
Laptop GPU.

## 4. Title key slots for XeKeys (`src/kernel/xboxkrnl/xboxkrnl_crypt.cpp`)

`XeKeysSetKey` and `XeKeysAesCbc` were stubs. Rock Band unwraps every song key
through them: an XDK wrapper calls `XeKeysSetKey(0xE0 + i, key, 16)`, then
`XeKeysAesCbc(0xE0 + i, block, 16, out, NULL, decrypt)`. The patch implements
slots 0xE0-0xE7 as plain AES-128-CBC (zero IV when `feed` is NULL) on top of the
SDK's existing XeCrypt helpers. Other key IDs (console fuses and keyvault) still
fail and log a warning.

The real hypervisor first deobscures a title's key with a console secret. That
step is not emulated, so the title has to be given deobscured keys. For Blitz
this is the app's `--title_keys=<64-byte file>` option. The keys never ship with
this project.

Measured with a temporary hook on libogg's `ogg_sync_pageseek` (since removed):
- Before: 0 Ogg pages decoded and about 5 MB skipped at boot. All music was
  silent, songs hung on LOADING, and LEADERBOARDS froze.
- After: about 3 pages per second at boot, 0 bytes skipped, and 6,014 pages
  over a full song.
- Emulating the deobscure step with the retail key 0x19 the SDK already has
  failed in all six variants (raw, AES-decrypt or AES-encrypt, each with a zero
  or chained IV), so that idea was dropped.

## 5. `k_32_32_32_32` textures (`src/graphics/...texture_cache`, new load shader)

Xenos format 35 (4×32-bit unsigned fraction) had no host format, so Xenia's
D3D12 backend skipped it. In Blitz, eight rotating 1024x16 linear data textures
use it every frame. Their texels are 32-bit UNORM values (0x80000000 = 0.5,
0xC0000000 = 0.75). Without them the note highway and all 3D models are missing:
[xenia game-compatibility #1533](https://github.com/xenia-project/game-compatibility/issues/1533).

The patch adds the `texture_load_r32g32b32a32_unorm_float` loader. It is the
128bpb loader plus `float(x) / (2^32 - 1)`, writing into `R32G32B32A32_FLOAT`,
so hardware filtering still works. Only the unsigned variant is mapped; signed
use is still reported as unsupported.

How the loader is built: the source `.cs.xesl` sits next to the bytecode. It
compiles against the XeSL includes of xenia-project/xenia `0b2ffa314853`, the
revision that rebuilds the SDK's own `texture_load_128bpb_cs.h` byte for byte.
The `fxc` command is in the header of the source file.

Measured: the "Unsupported texture formats ... k_32_32_32_32" error went from
every frame to 0. Gameplay screenshots now show the highway, notes, street
scene and characters.

## 6. Per-draw register trace, dev only (`src/graphics/command_processor.cpp`)

The oracle for the native renderer (docs/d3d-device-map.md). With
`--trace_draws=<file> --trace_draws_frames=<n,...>`, the command processor
writes one record per draw in the listed frames: the frame (swaps so far), the
draw packet's physical address, the VS and PS `IM_LOAD` addresses, and
registers `[0x2000, 0x2400)` and `[0x4000, 0x4928)`. Both flags are empty by
default, and then the only cost is one counter per swap and two stores per
shader load. `tools/device_map.py` pairs these records with the game-side
`--trace_device` log by packet address.

It lives in the stock GPU plugin, so it goes away with the plugin at phase N6.

## 7. Hidden window for automated runs, dev only (`src/ui/window_sdl.cpp`, `window.cpp`, `flags.h`)

`--window_hidden=true` skips `SDL_ShowWindow`, so a test run never puts a
window on screen or takes focus (the dev machine doubles as a TV). The game
renders as usual; `--screenshot_at` (src/diagnostics.cpp) saves frames from
the presenter instead of the screen. `scripts/run-scripted.ps1` passes it
unless `-Show`. Off by default.

## 8. Frame-exact captures of the stock renderer, dev only (`src/graphics/command_processor.cpp`)

The N5 gate compares the native renderer's frames with the stock renderer's at
the same game frame. The stock GPU runs behind the game, so a capture from the
game thread would get an older frame. The command processor counts the swaps
it executes (one per `D3DDevice_Swap`, the only `VdSwap` caller) and, for the
frames listed in the game's `--screenshot_frames` (read by name, re-read when
the game changes it), saves the frame it just presented as
`<--screenshot_prefix>-f<n>.bmp`. With the game's `--dump_memory` too, it also
writes guest physical memory as of that swap (`<prefix>-gpu-f<n>.bin`); with
`--readback_resolve=full` that holds the frame's resolves, which
`tools/compare_resolves.py` compares with the native renderer's, pass by pass.
Nothing happens when the flags are empty.

## 9. Audio frame clock, dev only (`src/audio/`, `src/kernel/xboxkrnl/xboxkrnl_video.cpp`)

Song time follows the audio clock, which follows the sound device, so two runs
of one input tape show different song positions at the same frame. With
`--audio_frame_clock=true`, the device gets silence, and each `VdSwap` plays
three of the game's 256-sample audio frames (a whole number, so positions do
not depend on the frame the audio started at) and waits until the game's audio
callbacks for them have returned. The audio worker no longer naps 500 ms when
idle in this mode: a nap made frames wait for it. Off by default.

## 10. Frame-locked guest clock, dev only (`src/core/clock.cpp`, `src/graphics/graphics_system.cpp`, `xboxkrnl_video.cpp`)

With `--clock_frame_lock=true`, guest time (the time base `mftb` reads, and
system time) is the presented frame count at 60 Hz, plus one tick per read so
waits on the clock still finish, from a fixed date (2012-06-19). Vblank timers
pace on real time instead (`Clock::QueryPacingTickCount`), or they would wait
for the frames they pace. With patch 9, two stock runs of the One Week mash
tape differ in 0–0.9% of pixels at the same distance from the first 3D frame
(hit sparks), against 37–39% without. Off by default; `scripts/gate-n5.ps1`
uses both.

## 11. Content from your own console (`src/kernel/xam/xam_content.cpp`, `src/system/xam/content_manager.cpp`)

Blitz's XEX lists Rock Band 1, 2 and 3 (45410829, 45410869, 45410914) as
alternate title IDs; that is how it plays your Rock Band song packages.
- `XamContentCreateEnumerator` also lists the alternate titles' content, as
  the console does (the SDK did that only for aggregate enumerators).
- `OpenContent` reads the license header under the package's own title, not
  the running title's.
- `InstallContent` (called by the game's `--install_content`) installs a package
  under its own title and content type from its STFS header. It skips the
  extraction when the package is already there, and only rewrites the header.
- License bits come only from the package's licenses that name an owner listed
  in `--content_owner_ids` (hex, comma-separated): a profile license (type
  0x0009, the XUID) or a console license (type 0xF000, the console ID). A
  package licensed to someone else installs with no license bits. The SDK used
  to grant every license entry.
- **Custom songs** (`--custom_songs_dir`, default `<user data>/custom_songs`):
  CON packages built with Magma or Onyx (their description starts "Created
  with Magma" or "Compiled by Onyx Music Game Toolkit") are listed as common content of their own title and type (customs
  use type 1) and mounted in place with `StfsContainerDevice`: no extraction.
  They get the license bits they declare. Anything else in the folder is
  skipped (logged at debug level). The game cannot create, delete or write
  thumbnails over these files: those calls return access denied.
  `IsCustomSongRoot` tells the game-side hook (`src/custom_song_checks.cpp`)
  which content roots are customs, so Blitz's chart checksum check is skipped
  for them only.
- Not fixed: type-1 content *installed* to the common (xuid 0) folder is listed
  but opened under the profile's folder, so it fails to open. Real DLC is
  type 2 (always common) and customs go through the custom-songs folder.

## 12. Playback device (`src/audio/sdl/sdl_audio_driver.cpp`)

Windows makes a USB DualSense (or DualShock 4) the default output when it is
plugged in, so the game played on the pad's speaker. Now:
- `--audio_device=<part of a name>` (case-insensitive) opens that device.
- Otherwise the system default, unless its name contains "DualSense" or
  "Wireless Controller": then the first other playback device. A name that
  matches nothing logs a warning and falls back to this.
- Checked with the pad as the Windows default: no option → Realtek speakers;
  `--audio_device=nosuchdevice` → Realtek; `--audio_device=dualsense` → the pad.
- With no option and no controller default, the game still opens SDL's default
  device, which follows Windows' default mid-game. A device picked by name or
  by skipping the pad stays put.

## 13. Settings file and key capture (`src/core/cvar.cpp`, `settings_overlay.{h,cpp}`)

- `SerializeToTOML` (behind `SaveConfig` and the F4 overlay's Save) wrote string
  values without escaping, so a Windows path (`C:\Users\...`) made a file that
  failed to load, losing every setting. Backslashes and quotes are now escaped.
- `ImGuiKeyToVirtualKey` (the F4 overlay's key capture) is exported, so the
  game's own hub (`src/hub.cpp`) rebinds keys the same way.

# XenosRecomp patch

`thirdparty/XenosRecomp` is a submodule pinned to hedge-dev/XenosRecomp
`990d03b` (MIT). `xenos-recomp.diff` adapts it to Blitz, everything under a
`BLITZ_RECOMP` define that `tools/xenos_recomp/CMakeLists.txt` sets:

```bash
git submodule update --init thirdparty/XenosRecomp
git -C thirdparty/XenosRecomp submodule update --init --depth 1 thirdparty/fmt thirdparty/xxHash thirdparty/zstd thirdparty/smol-v
git -C thirdparty/XenosRecomp apply ../../patches/xenos-recomp.diff
```

The `dxc-bin` submodule (prebuilt DXC binaries) is not needed: the wrapper
build uses `dxcompiler` and `dxil.dll` from the installed Windows SDK.

What the patch changes:
- **No constant table.** Blitz's shader containers have their reflection data
  stripped. The patch synthesizes a table in the same D3DX layout: the float4
  registers become arrays split around the shader's own literals (which stay
  locals), and every fetch slot and bool gets a name. The native renderer
  supplies the whole register file, as D3D does (docs/d3d-device-map.md), so
  names are irrelevant.
- **256 pixel shader constants and 32 fetch slots** (Unleashed: 224 and 16).
  Blitz's pixel shader literals reach c255, and its vertex shaders fetch the
  k_32_32_32_32 data textures from fetch slots 16–19. The shared-constants
  layout in `shader_common.h` moves accordingly.
- **Vertex texture fetch uses `SampleLevel(…, 0)`.** Vertex shaders have no
  derivatives, so `Sample` does not validate there.
- **Specialization constants are a root constant** (`b3, space4`) instead of
  DXIL library linking, so every shader compiles to one plain DXIL object.
- **`oPos.xy = oPos.xy * g_NdcScale + g_HalfPixelOffset * oPos.w`.** The
  renderer maps the guest viewport there when D3D12 cannot: screen-space draws
  (viewport transform off, positions in pixels, clipping off) and flipped
  viewports (positive Y scale). `g_HalfPixelOffset` carries the NDC offset and
  the Direct3D 9 half-pixel shift.
- **Vertex fetch in the shader.** Blitz's 3D shaders compute fetch indices
  (instancing from the vertex index, indices read from another stream), which
  an input assembler cannot express. Each fetch becomes
  `BlitzVertexFetch(n, r<src>.<c>)`: the guest streams are raw buffers
  (`t0-t3, space5`), and `g_VertexFetch[n]` (`b4, space4`) gives the fetch as
  D3D patched it at bind (format, stream, offset, stride, endian, swizzle,
  rounding), decoded as the SDK's shader interpreter does.
  `src/renderer/vertex_input.cpp` fills the descriptors. `r0.x` starts as the
  vertex index (`SV_VertexID + g_VertexIndexBase`), as on Xenos.
- **Cube direction from the swizzled operand.** `CUBEv` reads its direction as
  `(z, w, x)` of its first operand after the swizzle; XenosRecomp took the
  register's `.xyz`, which only works for the compiler layout `.zzxy`. Every cube
  fetch in Blitz uses another layout (`r2.yyxz`, `r9.zzwx`, ...), so every
  reflection was wrong before (the strikeline bar showed it).
