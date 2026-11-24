#!/usr/bin/env bash
# -DIMGUI=ON -DPT_PHYSICAL=ON

set -euo pipefail

repo_dir="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)"
build_dir="$repo_dir/build"

cmake -S "$repo_dir/neo" -B "$build_dir" -DCUDA_PATHTRACER=ON -DIMGUI=ON -DPT_PHYSICAL=ON "$@"
cmake --build "$build_dir"
