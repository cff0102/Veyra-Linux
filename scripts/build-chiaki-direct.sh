#!/usr/bin/env bash
set -euo pipefail
source_root="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/.." && pwd)"
local_root="${VEYRA_LOCAL_ROOT:-$(dirname -- "$source_root")/Veyra-local}"
deps="$local_root/deps"
chiaki="$deps/chiaki-ng-source"
headers="$deps/vulkan-headers"
mkdir -p -- "$deps"
if [[ ! -d "$chiaki/.git" ]]; then
    git clone --depth 1 --branch v1.10.1 https://github.com/chiaki-ng/chiaki-ng.git "$chiaki"
fi
[[ "$(git -C "$chiaki" rev-parse HEAD)" == f30da0aaeb96fc0026cc03683153f682352e7d7c ]] || { echo 'Unexpected Chiaki source revision'; exit 1; }
git -C "$chiaki" submodule update --init --depth 1 third-party/nanopb third-party/jerasure third-party/gf-complete
patch="$source_root/linux/chiaki/chiaki-ng-v1.10.1.patch"
if ! git -C "$chiaki" apply --reverse --check "$patch" 2>/dev/null; then
    git -C "$chiaki" apply --check "$patch"
    git -C "$chiaki" apply "$patch"
fi
if [[ ! -d "$headers/.git" ]]; then
    git clone --depth 1 --branch v1.3.290 https://github.com/KhronosGroup/Vulkan-Headers.git "$headers"
fi
[[ "$(git -C "$headers" rev-parse HEAD)" == b379292b2ab6df5771ba9870d53cf8b2c9295daf ]] || { echo 'Unexpected Vulkan headers revision'; exit 1; }
if [[ ! -x "$deps/chiaki-build-python/bin/python" ]]; then
    uv venv --python 3.12 "$deps/chiaki-build-python"
fi
uv pip install --python "$deps/chiaki-build-python/bin/python" protobuf==7.36.2
cmake -S "$chiaki" -B "$local_root/build/chiaki-direct" -G Ninja \
    -DCMAKE_BUILD_TYPE=Release -DCMAKE_POLICY_VERSION_MINIMUM=3.5 \
    -DCHIAKI_ENABLE_TESTS=OFF -DCHIAKI_ENABLE_STEAMDECK_NATIVE=OFF \
    -DCHIAKI_ENABLE_STEAM_SHORTCUT=OFF -DCHIAKI_USE_SYSTEM_CURL=ON \
    -DCHIAKI_ENABLE_SETSU=OFF -DCHIAKI_ENABLE_PI_DECODER=OFF \
    -DPYTHON_EXECUTABLE="$deps/chiaki-build-python/bin/python" \
    -DVulkan_INCLUDE_DIR="$headers/include" \
    -DCMAKE_C_FLAGS="-I$headers/include" -DCMAKE_CXX_FLAGS="-I$headers/include" \
    -DVEYRA_FRAME_BRIDGE_DIR="$source_root/linux/chiaki"
timeout 900 cmake --build "$local_root/build/chiaki-direct" --target chiaki -j2
