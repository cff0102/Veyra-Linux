# Veyra Linux Preview

An experimental native Linux player derived from [Likely7/Veyra-NRVideo](https://github.com/Likely7/Veyra-NRVideo), with **NVIDIA video frame generation over CUDA/OpenGL**. No Windows or Wine is used by the new player.

This is an early, partial port, not a feature-complete Linux release of upstream Veyra. The upstream Windows source, history and licenses are retained; its documentation is in [README_EN.md](README_EN.md).

## Available in the native player

- Local video playback through Qt Multimedia.
- Linux camera/capture-device and format selection.
- Screen/window sharing for an external Remote Play client, using Qt's capture backend.
- NVIDIA VFG **2× and 4×**, low/medium/high model selection, automatic scene-cut handling.
- GPU output through CUDA/OpenGL interop, optional sharpening and fullscreen viewing.
- Bounded frame mailboxes and explicit input/generated/submitted/drop counters.

Built and tested on **CachyOS x86-64 with an RTX 5080**. Other distributions are intended targets, but have not been validated. The NVIDIA runtime, GPU driver, Qt version and desktop capture support matter more than the distro name.

## Build and run

Requires a C++20 compiler, CMake 3.24+, Ninja, Python 3.11+, Qt 6.8+ Widgets/Multimedia/OpenGLWidgets, and an installed NVIDIA driver. Frame generation requires an NVIDIA GPU supported by the Linux VFX runtime.

From the repository root:

```sh
python scripts/prepare-linux-runtime.py
cmake -S . -B ../Veyra-local/build/native -G Ninja \
  -DCMAKE_BUILD_TYPE=Release \
  -DVEYRA_BUILD_OBS_FILTER=OFF \
  -DVEYRA_BUILD_LINUX_PLAYER=ON \
  -DVEYRA_CUDA_INCLUDE_DIR="$PWD/../Veyra-local/deps/cuda/nvidia/cuda_runtime/include"
cmake --build ../Veyra-local/build/native -j2
./scripts/run-linux.sh --demo --multiplier 4
```

The preparation script downloads checksum-pinned, unmodified official NVIDIA wheels into the sibling `Veyra-local` directory. It retains their licenses and provenance. It does not install Python packages, change the GPU driver, or install anything system-wide. NVIDIA runtimes/models are **not included in this repository**.

Launch without `--demo` to select a video or capture source. Frame generation defaults off; verify your source first, then enable 2× or 4×. Press **F11** for picture-only fullscreen and **Esc** to return.

See [Linux setup, PS5 connections and testing](docs/LINUX_PLAYER.md).

## Current limits

- **SDR only.** HDR input is rejected when identified by the source metadata.
- DLSS neural rendering, AI super resolution, HDR processing, built-in PS5 pairing, node editing and export have not been ported to this player.
- PS5 Remote Play requires a separate client such as chiaki-ng. Physical PS5/capture-card and Remote Play integration have not yet been validated in this fork.
- Frame generation adds latency and can introduce artifacts. It does not change the console's game simulation or control-response rate. Submitted frames are not measured physical display FPS.
- Audio monitoring has no automatic compensation for the added video delay.
- The current prototype maps input frames through Qt/CPU memory, loads the model on the GUI thread and has not been tuned for long sessions.

The existing [OBS filter](docs/OBS_FILTER.md) remains a separate build with spatial effects; it does not provide frame generation.

## License and attribution

This work builds on Veyra by Likely7 and its contributors. Preserve [LICENSE](LICENSE), [THIRD_PARTY_NOTICES.md](THIRD_PARTY_NOTICES.md) and the component licenses. NVIDIA's separately downloaded runtime has its own software/model terms. This project is not affiliated with NVIDIA, Sony or Rockstar Games.
