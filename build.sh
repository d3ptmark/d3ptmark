#!/usr/bin/env bash
# Builds d3ptmark-le (legacy renderer) and d3ptmark-pt (CUDA path tracer) into build/.
# Extra arguments are passed to CMake, e.g.:
#   ./build.sh -DPATHTRACED=OFF
#   ./build.sh -DCMAKE_CUDA_ARCHITECTURES=75

set -euo pipefail

repo_dir="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)"
build_dir="$repo_dir/build"

cmake -S "$repo_dir/neo" -B "$build_dir" "$@"
cmake --build "$build_dir" --parallel "$(nproc)"
