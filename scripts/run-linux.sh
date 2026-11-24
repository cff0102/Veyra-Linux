#!/usr/bin/env bash
set -euo pipefail
script_dir="$(cd -- "$(dirname -- "$(readlink -f -- "${BASH_SOURCE[0]}")")" && pwd)"
source_dir="$(cd -- "$script_dir/.." && pwd)"
local_root="${VEYRA_LOCAL_ROOT:-$(dirname -- "$source_dir")/Veyra-local}"
binary="${VEYRA_LINUX_BINARY:-$local_root/build/native/linux/player/veyra-linux}"
runtime="${VEYRA_VFG_RUNTIME:-$local_root/deps/vfx/nvvfx/libs}"
if [[ ! -x "$binary" ]]; then
    printf 'Native player has not been built: %s\nSee docs/LINUX_PLAYER.md.\n' "$binary" >&2
    exit 1
fi
# Applies only to this process and its children. No driver/runtime replacement.
mkdir -p -- "$local_root/cache/cuda"
exec env LD_LIBRARY_PATH="$runtime${LD_LIBRARY_PATH:+:$LD_LIBRARY_PATH}" \
    CUDA_CACHE_PATH="$local_root/cache/cuda" \
    "$binary" --runtime "$runtime" "$@"
