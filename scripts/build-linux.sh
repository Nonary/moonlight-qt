#!/usr/bin/env bash
set -euo pipefail

repo_root=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/.." && pwd -P)
config=${1:-release}

if [[ $# -gt 1 || ( $config != release && $config != debug ) ]]; then
    echo "Usage: $0 [release|debug]" >&2
    exit 2
fi

for tool in qmake6 make python3; do
    if ! command -v "$tool" >/dev/null 2>&1; then
        echo "Missing build tool: $tool" >&2
        exit 1
    fi
done

jobs=${BUILD_JOBS:-$(python3 "$repo_root/scripts/linux-build-budget.py" --jobs)}
if [[ ! $jobs =~ ^[1-9][0-9]*$ ]]; then
    echo "BUILD_JOBS must be a positive integer" >&2
    exit 2
fi

build_dir="$repo_root/build/local-linux-$config"
mkdir -p "$build_dir"

echo "Configuring $config build in $build_dir"
(
    cd "$build_dir"
    qmake6 -r "$repo_root/moonlight-qt.pro" "CONFIG+=$config"
    make -j "$jobs" "$config"
)

echo "Build complete: $build_dir/app/moonlight"
