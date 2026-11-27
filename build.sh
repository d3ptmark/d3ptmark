#!/usr/bin/env bash
# Builds d3ptmark-le (OpenGL + path tracer without PT_PHYSICAL) and d3ptmark-pt (path tracer with PT_PHYSICAL) into build/.
# Extra arguments are passed to CMake, e.g.:
#   ./build.sh -DPATHTRACED=OFF
#   ./build.sh -DCMAKE_CUDA_ARCHITECTURES=75

set -euo pipefail

repo_dir="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)"
build_dir="$repo_dir/build"

cmake -S "$repo_dir/neo" -B "$build_dir" "$@"
cmake --build "$build_dir" --parallel "$(nproc)"
