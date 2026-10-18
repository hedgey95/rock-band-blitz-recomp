# Rock Band Blitz for PC (static recompilation)

A native Windows build of **Rock Band Blitz** (Xbox 360, XBLA), made by
statically recompiling the game's own code with the
[ReXGlue SDK](https://github.com/rexglue/rexglue-sdk) and drawing it with a
native Direct3D 12 renderer that takes over the game's D3D calls.

> **You need your own copy of the game.** This repository contains no game
> files, no keys and no recompiled game code. You supply the game package (and
> the song decryption keys) from your own copy; the build recompiles it on your
> machine. Not affiliated with or endorsed by Harmonix, MTV Games or Microsoft.

## What works

- Songs play start to finish, with menus, the highway, the 3D city and audio.
- Native D3D12 renderer, checked against Xenia's renderer frame by frame
  (`scripts/gate-n5.ps1`: every frame within 1% of pixels).
- Controllers (tested with a PS5 DualSense) and the keyboard.
- An in-game hub (F1) for the songs folder, audio output, renderer,
  fullscreen and key bindings; the game starts from a double-click.
- Custom songs (Magma or Onyx CON packages) from a folder, and song packages
  copied from your own console. See [docs/content-packages.md](docs/content-packages.md).

How to play: [docs/playing.md](docs/playing.md).

## Building

Windows 10 or 11 (x64) with a Direct3D 12 GPU, and:

- LLVM 20.1.8 (clang), CMake 4.x, Ninja (the SDK's toolchain; all available from winget)
- The Windows SDK (its `dxc` compiles shaders, in the build and in the game)
- Git, and Python 3 for the optional tools in `tools/`

### 1. Sources

```bat
git clone https://github.com/hedgey95/rock-band-blitz-recomp.git rbb-recomp
cd rbb-recomp
git submodule update --init thirdparty/XenosRecomp
git -C thirdparty/XenosRecomp submodule update --init --depth 1 thirdparty/fmt thirdparty/xxHash thirdparty/zstd thirdparty/smol-v
git -C thirdparty/XenosRecomp apply ../../patches/xenos-recomp.diff
```

(XenosRecomp's `dxc-bin` submodule is not needed: the Windows SDK's `dxc` is used.)

### 2. ReXGlue SDK (pinned and patched)

```bat
git clone --branch v0.10.0 --recursive https://github.com/rexglue/rexglue-sdk.git sdk
git -C sdk apply ../patches/rexglue-sdk.diff
cmake -S sdk --preset win-amd64
cmake --build sdk/out/build/win-amd64 --config Release --target install
cmake --build sdk/out/build/win-amd64 --config RelWithDebInfo --target install
```

The Release build installs the `rexglue` command-line tool; the game links
the RelWithDebInfo runtime. What the patch changes, and why:
[patches/README.md](patches/README.md).

### 3. Your game files

Copy the Rock Band Blitz package from your console to the PC, then unpack it
with `tools/extract_package`, a small tool built on the SDK:

```bat
cmake -S tools/extract_package -B out/build/extract_package -G Ninja -DCMAKE_CXX_COMPILER=clang++ -DCMAKE_BUILD_TYPE=RelWithDebInfo -Drexglue_DIR=%CD%/sdk/out/install/win-amd64/lib/cmake/rexglue
cmake --build out/build/extract_package
out\build\extract_package\extract_package.exe <package file> D:\path\to\unpacked-blitz
```

Make `assets` in the repository point at the unpacked folder (the one with
`default.xex`):

```bat
mklink /J assets D:\path\to\unpacked-blitz
```

### 4. Recompile and build

```bat
sdk\out\install\win-amd64\bin\rexglue.exe codegen blitz_manifest.toml
cmake --preset win-amd64-relwithdebinfo -Drexglue_DIR=%CD%/sdk/out/install/win-amd64/lib/cmake/rexglue
cmake --build out/build/win-amd64-relwithdebinfo
```

`rexglue codegen` writes the recompiled C++ to `generated/default` (about
110 MB, never committed).

### 5. First run

Create `blitz.toml` next to `blitz.exe` (`out/build/win-amd64-relwithdebinfo`):

```toml
game_data_root = "D:\\path\\to\\unpacked-blitz"
title_keys = "D:\\path\\to\\title_keys.bin"
gpu_plugin = "xenos"
native_renderer = true
native_shader_dir = "D:\\path\\to\\shader-cache"
```

- `title_keys`: the game's song audio is encrypted with per-title keys that a
  real console derives in its hypervisor, which is not emulated. You supply
  them as a 64-byte file of deobscured keys; without it the game runs silent.
- `native_shader_dir`: any empty folder. The first launch translates the
  game's shaders into it (in the background while the game loads, under a
  minute); later launches load them from there.

Then double-click `blitz.exe`. The hub opens on first launch; everything else
can be set there. Command-line flags (`--name=value`) override `blitz.toml`.

## Documentation

- [docs/playing.md](docs/playing.md): the hub, keyboard layout, settings
- [docs/content-packages.md](docs/content-packages.md): custom songs and your own DLC
- [docs/native-renderer-plan.md](docs/native-renderer-plan.md): how the renderer works, phase by phase, with measurements
- [docs/d3d-device-map.md](docs/d3d-device-map.md): the game's D3D device, as the renderer reads it
- [patches/README.md](patches/README.md): every SDK and XenosRecomp change and why

## Credits

- [ReXGlue SDK](https://github.com/rexglue/rexglue-sdk) (recompiler, kernel, audio, input; built on Xenia)
- [XenosRecomp](https://github.com/hedge-dev/XenosRecomp) (Xenos shader translation, MIT)
- Rock Band Blitz is © Harmonix Music Systems. This project is for playing
  the copy you own.

## Licence

This project's own code and documentation are under the [MIT licence](LICENSE).
That covers nothing from the game, the ReXGlue SDK (its own licence) or
XenosRecomp (MIT); the patches in `patches/` modify those projects and follow
their licences.
