#!/usr/bin/env bash
# Removes the build/ directory.

set -euo pipefail

repo_dir="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)"
rm -rf "$repo_dir/build"
echo "removed $repo_dir/build"
