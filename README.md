# d3ptmark — Doom 3 Path-Tracer Benchmark

**d3ptmark** is a benchmark mod for **Doom 3** that renders the game with a real-time
**GPU path tracer** and compares it, frame by frame, against the classic OpenGL renderer.

It is a fork of [dhewm3](https://github.com/dhewm/dhewm3), the Doom 3 GPL source port.
All engine credit goes to id Software and the dhewm3 contributors; everything under
`neo/renderer_pt/` and the benchmark tooling is what this project adds.

One build produces two binaries from the same source tree:

| Binary | Renderer |
|--------|----------|
| `d3ptmark-le` | **l**egacy OpenGL rasterizer (stock dhewm3 rendering) |
| `d3ptmark-pt` | CUDA **p**ath **t**racer (compiled with `-DPT_PATHTRACED`) |

Both binaries share the same game libraries (`base`, `d3xp`) and the same game data,
so like-for-like comparisons only swap the executable.

## Requirements

- **Doom 3 game data** (the `*.pk4` files of a retail/Steam/GOG install, patched to
  1.3.1) — not included
- Linux (GCC/Clang) or Windows (Visual Studio)
- CMake ≥ 3.18, SDL2, OpenAL, optionally libcurl
- For `d3ptmark-pt`: the NVIDIA CUDA toolkit and an NVIDIA GPU (Turing / SM 75 or newer)

## Building

Linux:

```sh
./build.sh      # configure + build into build/
./clean.sh      # remove build/
```

Windows (CMake and Visual Studio installed, run from a developer prompt):

```bat
build.bat -DDHEWM3LIBS=C:\path\to\dhewm3-libs\x86_64-w64-mingw32
clean.bat
```

Prebuilt SDL2/OpenAL/curl for Windows come from the
[dhewm3-libs](https://github.com/dhewm/dhewm3-libs) repository.

Binaries end up in `build/` (Windows: `build\RelWithDebInfo\`). Extra script
arguments are passed to CMake:

| Option | Default | Effect |
|--------|---------|--------|
| `-DPATHTRACED=OFF` | `ON` | skip `d3ptmark-pt` (no CUDA toolkit required) |
| `-DLEGACY=OFF` | `ON` | skip `d3ptmark-le` |
| `-DTESTS=ON` | `OFF` | build the path tracer regression tests (`ctest`) |
| `-DCMAKE_CUDA_ARCHITECTURES=75` | `75;80;86;89;90` | build kernels for one GPU generation only (faster build) |

If your host compiler is newer than your CUDA toolkit supports, configure with the
`CUDAFLAGS` environment variable, e.g.:

```sh
CUDAFLAGS="-allow-unsupported-compiler -std=c++17" ./build.sh
```

See [docs/PathTracer.md](./docs/PathTracer.md) for more CUDA build details.

## Running

Copy the Doom 3 `*.pk4` files into `base/` (and `d3xp/` for the expansion) next to the
binaries, or point the engine at an existing installation:

```sh
./build/d3ptmark-pt +set fs_basepath /path/to/doom3
./build/d3ptmark-le +set fs_basepath /path/to/doom3
```

## Benchmarking

Quality tiers are selected with `r_ptPreset` (`gameplay_low/med/high/ultra`,
`cinematic_low/med/high/ultra`). The main benchmark commands:

| Command | Description |
|---------|-------------|
| `r_ptBenchmark <demo> <spp>` | play a recorded render demo, converging every frame to exactly `<spp>` samples per pixel, saving each frame and printing timing statistics |
| `r_glBenchmark <demo>` | replay the same demo with OpenGL for image-by-image comparison (also available in `d3ptmark-le`) |
| `r_ptSequence <start> <end> [spp]` | path trace a range of live game ticks at exact samples per pixel |
| `r_ptCompare [spp]` | save an OpenGL and a path-traced screenshot of the current view |
| `r_ptInfo` | print backend, device, memory and timing info |
| `r_ptAbort` / `r_glAbort` | cancel a running capture, keeping finished images |

Example run:

```sh
./build/d3ptmark-pt +r_ptPreset cinematic_high +r_ptBenchmark demo1 32
```

Images and versioned capture reports are written below `screenshots/` in `fs_savepath`
(`ptbench/`, `glbench/`, `ptreports/`, `glreports/`). With Dear ImGui enabled, `r_ptUI`
opens the in-game tuning panel for presets, quality budgets, denoising and the physical
camera/material controls.

## Documentation

- [docs/PathTracer.md](./docs/PathTracer.md) — path tracer internals: architecture,
  rendering modes, materials, denoising, physical rendering, presets, console
  commands and CVar reference
- [docs/README-dhewm3.md](./docs/README-dhewm3.md) — the upstream dhewm3 README,
  including notes on game data and third-party license notices
- [Configuration.md](./Configuration.md) — engine configuration
- [Changelog.md](./Changelog.md) — dhewm3 changelog

## License

GPLv3, see [COPYING.txt](./COPYING.txt). Based on [dhewm3](https://github.com/dhewm/dhewm3);
Doom 3 is a trademark of id Software. Third-party license notices are listed in
[docs/README-dhewm3.md](./docs/README-dhewm3.md).
