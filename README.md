# d3ptmark — Doom 3 Path-Tracer Benchmark

**d3ptmark** renders **Doom 3** with a real-time **GPU path tracer** and compares it, frame by
frame, against the classic OpenGL renderer. It is a fork of [dhewm3](https://github.com/dhewm/dhewm3),
the Doom 3 GPL source port. All engine credit goes to id Software and the dhewm3 contributors;
this project adds `neo/renderer_pt/` and the benchmark tooling.

One build produces two binaries that share the game libraries and data:

| Binary | Renderer |
|--------|----------|
| `d3ptmark-le` | OpenGL, plus the path tracer without the physical renderer |
| `d3ptmark-pt` | Path tracer with the experimental physical renderer (`PT_PHYSICAL`) |

## Requirements

- Doom 3 game data (`*.pk4` from a retail/Steam/GOG install, patched to 1.3.1), not included
- Linux (GCC/Clang) or Windows (Visual Studio), CMake ≥ 3.18, SDL2, OpenAL, optionally libcurl
- For the path tracer, at least one of:
  - CUDA toolkit and an NVIDIA GPU (Turing / SM 75 or newer)
  - OpenCL headers (`CL/cl.h`) and an OpenCL 1.2 GPU (AMD, Intel or NVIDIA)

## Building

```sh
./build.sh      # Linux: configure and build into build/
./clean.sh
```

```bat
build.bat -DDHEWM3LIBS=C:\path\to\dhewm3-libs\x86_64-w64-mingw32
clean.bat
```

On Windows, run from a Visual Studio developer prompt. SDL2/OpenAL/curl come from
[dhewm3-libs](https://github.com/dhewm/dhewm3-libs). Binaries land in `build/`
(`build\RelWithDebInfo\` on Windows). Extra arguments are passed to CMake:

| Option | Default | Effect |
|--------|---------|--------|
| `-DPATHTRACED=OFF` | `ON` | Skip `d3ptmark-pt` |
| `-DLEGACY=OFF` | `ON` | Skip `d3ptmark-le` |
| `-DOPENCL=OFF` | `ON` | Build without the OpenCL backend |
| `-DTESTS=ON` | `OFF` | Build the regression tests (`ctest`) |
| `-DCMAKE_CUDA_ARCHITECTURES=75` | `75;80;86;89;90` | Build kernels for one GPU generation |

If your compiler is newer than your CUDA toolkit supports, see
[docs/PathTracer.md](./docs/PathTracer.md#cuda).

## Running

Copy the `*.pk4` files into `base/` (and `d3xp/` for the expansion) next to the binaries, or
point the engine at an installation:

```sh
./build/d3ptmark-pt +set fs_basepath /path/to/doom3
```

## Benchmarking

```sh
./build/d3ptmark-pt +r_ptPreset cinematic_high +r_ptBenchmark demo1 32
```

| Command | Description |
|---------|-------------|
| `r_ptBenchmark <demo> <spp> [uncapped] [save_image 0\|1]` | Replay a render demo, converging every frame to exactly `<spp>` samples per pixel |
| `r_glBenchmark <demo> [uncapped] [save_image 0\|1]` | Replay the same demo with OpenGL for image-by-image comparison |
| `r_ptSequence <start> <end> [spp]` | Path trace a range of live game ticks |
| `r_ptCompare [spp]` | Save an OpenGL and a path-traced screenshot of the current view |
| `r_ptAbort` / `r_glAbort` | Cancel a capture, keeping finished images |
| `r_ptInfo` | Backend, device, memory and timing info |

By default playback is capped at the demo's 30 Hz and every frame is saved as an image.
`uncapped` runs as fast as the GPU allows (no 30 Hz cap, vsync off for the run) and
`save_image 0` skips the image writes, so `uncapped save_image 0` measures pure throughput:

```sh
./build/d3ptmark-pt +r_ptPreset gameplay_med +r_ptBenchmark demo1 1 uncapped save_image 0
```

### Headless (servers, CLI)

`--headless` runs without a window and without a display (no X11/Wayland session needed):
OpenGL goes through SDL's offscreen driver (EGL), sound is off, and the engine quits when
the benchmark ends with exit code 0 (completed), 1 (aborted/failed) or 2 (did not start):

```sh
./build/d3ptmark-pt --headless +set fs_basepath /path/to/doom3 \
    +set r_mode -1 +set r_customWidth 1920 +set r_customHeight 1080 \
    +r_ptBenchmark demo1 32 uncapped save_image 0
```

Quality is set with `r_ptPreset` (`gameplay_low/med/high/ultra`, `cinematic_low/med/high/ultra`).
Images and capture reports (timings plus GPU/CPU telemetry) are written below `screenshots/`
in `fs_savepath`. `r_ptUI` opens the in-game tuning panel.

## Documentation

- [docs/PathTracer.md](./docs/PathTracer.md): path tracer building, architecture,
  rendering, denoising, physical renderer, capture tools, presets and cvars
- [Benchmark.md](./Benchmark.md): benchmark runs and timing details
- [docs/README-dhewm3.md](./docs/README-dhewm3.md): upstream dhewm3 README and third-party notices
- [Configuration.md](./Configuration.md): engine configuration
- [Changelog.md](./Changelog.md): dhewm3 changelog

## License

GPLv3, see [COPYING.txt](./COPYING.txt). Doom 3 is a trademark of id Software. Third-party
notices are listed in [docs/README-dhewm3.md](./docs/README-dhewm3.md).
