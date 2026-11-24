# Native Linux preview

This fork adds an independent Qt Multimedia/OpenGL executable under `linux/player`.
It is a partial port focused on viewing captured video with NVIDIA VFX frame
generation. The original D3D12 application remains Windows-specific.

## Distribution and GPU requirements

The only locally tested configuration is CachyOS x86-64, Qt 6.12, NVIDIA RTX 5080,
driver 615.78.08. Other distributions are not yet certified by this project.

The code requires Qt 6.8 or newer, including Widgets, Multimedia, OpenGLWidgets,
a working multimedia backend, and desktop OpenGL 3.3. A working NVIDIA driver
providing `libcuda.so` is required. Compilation uses public CUDA driver headers;
it does not require `nvcc`. The preparation script supplies headers separately
from the installed driver. On Wayland, install Qt's Wayland plugin and an
appropriate desktop portal/PipeWire backend for screen sharing.

On Arch-based systems the development packages normally include `base-devel`,
`cmake`, `ninja`, `python`, `qt6-base`, `qt6-multimedia` and `qt6-wayland`. Use your
distribution's package manager for equivalents. Some stable distributions ship
Qt older than 6.8 and need a newer Qt installation. A binary compiled on CachyOS
is not promised to run unchanged on older distributions: build from source.

NVIDIA documents Linux x86-64 VFG support for Ada, Hopper and Blackwell GPUs.
The Linux SDK targets server deployments; CachyOS desktop use is outside its
published qualified OS/deployment list. Local RTX 5080 tests establish this
machine's operation, not support for every NVIDIA GPU or distribution:

- [VFG API and GPU requirements](https://docs.nvidia.com/maxine/vfx/latest/Filters/VideoFrameGeneration.html)
- [NVIDIA Linux requirements](https://docs.nvidia.com/maxine/vfx/latest/LinuxVFXSDK/GetStartedonLinux.html)

## Reproducible local setup

Run the commands in the root README. All downloaded and generated files go in
`../Veyra-local`, outside Git. The runtime script verifies these official wheels:

| Component | Version | SHA-256 |
| --- | --- | --- |
| NVIDIA VFX, Linux x86-64 cp312 abi3 wheel | 0.2.0.0 | `0252866a7b798a6f92c8543443d88f716e8c5bbc6a7ddd77a2754728dada9fa8` |
| NVIDIA CUDA runtime/header wheel, Linux x86-64 | 12.9.79 | `25bba2dfb01d48a9b59ca474a1ac43c6ebf7011f1b0b8cc44f54eb6ac48a96c3` |

The player calls the native libraries directly; it does not import the wheel's
Python extension. The wheel's CPython version therefore does not constrain the
C++ executable. Licenses stay in the extracted `.dist-info/licenses` directories.

`run-linux.sh` scopes library and CUDA cache paths to the child process. Override
`VEYRA_LOCAL_ROOT`, `VEYRA_LINUX_BINARY` or `VEYRA_VFG_RUNTIME` for a different
layout. The runtime override must point to an absolute directory containing
`libVideoFX.so`, `libNVCVImage.so`, the VFG plugin and its dependencies. Do not
mix Windows DLLs with Linux `.so` files.

The OBS filter remains enabled by default in the upstream CMake Linux branch.
Set `VEYRA_BUILD_OBS_FILTER=OFF` for the standalone player without libobs.

## PS5 with an HDMI capture card

1. Use a Linux-compatible capture card whose driver exposes a supported video
   device. Verify its **capture** resolution/rate, not just HDMI passthrough.
2. With the PS5 connected to your normal display, turn off **Settings → System →
   HDMI → Enable HDCP**. Start with SDR and a card-supported signal, for example
   1080p60. Keep HDR and VRR off while establishing a reliable baseline.
3. Wire **PS5 HDMI OUT → capture card HDMI IN**, and the card's USB connection to
   a suitable PC USB port. A PCIe card uses its PCIe connection instead.
4. Connect your monitor to the **PC's GPU** and view the Linux player's output.
   The capture card's HDMI OUT is optional original-signal passthrough; it does
   not contain the player's generated frames. A GPU HDMI port is an output,
   so connecting the PS5 directly to it does not capture video.
5. Open the player, refresh devices, choose the card and a supported format,
   then click **Open capture card**. Select the card's audio input if needed.
6. Verify picture, sound and controller operation with frame generation off.
   Keep the DualSense connected to the PS5. Then try VFG 2× before 4×.
7. Use a high-refresh monitor configured at the desired refresh rate. A 60 Hz
   display cannot show 120 distinct frames per second.

Sony's HDCP setting and the physical connections are also described in
[Elgato's PS5 setup instructions](https://help.elgato.com/hc/en-us/articles/5329003032973-Elgato-Game-Capture-HD60-X-PlayStation-5-Setup).
This link illustrates wiring; it is not a Linux-driver endorsement of that card.

## PS5 over Remote Play

A capture card is optional. This first port uses an external Remote Play client,
not Veyra's Windows/Chiaki integration:

1. Connect PS5 and PC to your local network; wired Ethernet is preferable for
   consistent latency. Enable **Settings → System → Remote Play** on the PS5.
2. Install and pair [chiaki-ng](https://github.com/chiaki-ng/chiaki-ng), following
   its [setup documentation](https://chiaki-ng.github.io/chiaki-ng/setup/).
   Complete pairing locally; do not publish account IDs or pairing credentials.
3. Verify its picture, sound and controller first. Select an SDR stream.
4. Run chiaki-ng and Veyra using X11/XWayland and choose **Capture window**.
   Select the Remote Play window. Qt can display a fallback `Window 0x...` name
   for clients that do not set a legacy X11 title. **Share screen** is a separate
   whole-display capture control; its Wayland portal does not offer windows.
5. Audio remains in chiaki-ng. Keep its window open on the current desktop.
   Check controller input with Veyra in front. Do not capture Veyra itself or
   a display containing its preview, which creates a feedback loop.

For a window with the exact title `chiaki-ng`, this starts a 30 FPS capture with
4× generation and picture-only fullscreen (the rate limit requires Qt 6.12+):

```sh
QT_QPA_PLATFORM=xcb XDG_SESSION_TYPE=x11 ./scripts/run-linux.sh \
  --capture-window chiaki-ng --capture-fps 30 --multiplier 4 --fullscreen
```

The environment overrides apply only to the viewer process, including when the
desktop itself runs Wayland. This asynchronous capture is not synchronized to
the console and may repeat or miss a frame. These are requested processing
rates, not verified physical display rates. A missing or ambiguous title
produces a status message rather than selecting another window.

For interactive play, prefer this responsiveness profile over 30 FPS capture:

```sh
QT_QPA_PLATFORM=xcb XDG_SESSION_TYPE=x11 ./scripts/run-linux.sh \
  --capture-window chiaki-ng --capture-fps 60 --multiplier 2 \
  --latency-priority --fullscreen
```

Capturing a 60 FPS Remote Play window at 60 rather than 30 FPS reduces the wait
for its next sample even if the game renders at 30 FPS. The tradeoff is less
useful interpolation between the game's unique frames; a repeated source frame
does not become new game motion. The latency-priority scheduler skips the old
endpoint, anchors deadlines before conversion/inference, and skips intermediate
frames whose deadlines have passed. The command-line flag also requests swap
interval zero; the compositor still controls screen presentation. The checkbox
changes scheduling only; swap interval is selected when the app starts.

Frame delivery uses a coalesced event and latest-frame mailbox instead of a
polling timer. Neither this nor frame generation can match native Remote Play's
latency: capture and interpolation add work and interpolation requires a later
source frame. Use chiaki-ng directly when minimum latency matters more than
smoothing.

Add `--timing-log` to record Qt frame-receipt-to-swap measurements. `content-age`
estimates the timestamp of the displayed interpolated content from its two
source receipts; `latest-receipt-to-swap` measures time since the newer source
receipt. Both exclude network/decode, capture sampling delay, and physical
screen scanout. They are diagnostics, not controller-to-screen latency.

## Current implementation boundaries

- Qt supplies files, camera frames and desktop capture. Input is converted to
  RGBA8 through CPU-visible Qt memory. This is not a zero-copy capture pipeline.
- The native VFG adapter follows the public C API sequencing used by upstream
  `src/pipeline/VfgBackend.cpp`: explicit previous/current images, model mode,
  multiplier/index, automatic/manual shot-change controls.
- Owned pinned staging uploads input asynchronously. CUDA writes generated
  frames to registered OpenGL textures. There is no generated-pixel readback in
  normal playback. Self-tests deliberately read pixels for verification.
- A latest-frame mailbox bounds source buffering; the display batch is at most
  five textures. Overload drops source frames instead of accumulating latency.
  Exact duplicate detection does not detect near-duplicates from lossy streams.
- Frames with a backwards timestamp, a gap over 100 ms, changed dimensions or a
  changed source invalidate the interpolation pair. Source/setting changes
  release the backend; capture gaps do not repeatedly reload the model.
- Model loading runs on the GUI thread and can briefly freeze controls. The
  initial shader, decoder and model warmup are not steady-state performance.
- HDR is unsupported; PQ/HLG metadata is rejected. Incorrectly tagged sources
  still require manual SDR configuration. No native game motion vectors/depth
  are available from a PS5 video capture.
- Audio is monitored directly. There is no automatic A/V delay compensation,
  export pipeline, native console pairing or full upstream QML interface here.

## Validation

Run a GPU test without connecting to any console:

```sh
./scripts/run-linux.sh --gpu-self-test --test-output ../Veyra-local/tests/gpu
./scripts/run-linux.sh --demo --multiplier 4 --quit-after 12
```

The GPU test checks 2× and 4× generation, exact source RGBA identity, the expected
positions of an interpolated moving object, scene-cut bypass, repeated calls and
resource teardown. It also times 30 warmed 1080p 4× low-model pairs, including
host upload, inference and CUDA-to-GL copies.

On the tested RTX 5080, generated object centers were 240.98, 249.38 and 258.45
pixels against expected 239.5, 249.5 and 259.5 pixels. The 1080p processing sample
averaged 2.28 ms/pair (max 2.34 ms). These are synthetic processing measurements,
not physical 120 Hz display verification or capture-to-display latency.

Real PS5 Remote Play window capture was also smoke-tested on this machine with
GTA VI: a 12-second run received 357 frames and generated 1,062 intermediate
frames, and a screenshot confirmed the game picture in Veyra. This verifies
capture and active generation, not a steady 120 FPS display rate, end-to-end
latency, interpolation quality, or long-session reliability. HDMI capture cards,
other GPUs and other compositors remain unverified.

A local latency comparison on the same live PS5 window measured approximately
43 ms estimated content age with 30 FPS capture / 4× / normal scheduling, versus
12 ms with 60 FPS capture / 2× / latency priority and event-driven delivery.
The latter showed approximately 8 ms from latest frame receipt to Qt swap,
and zero source-mailbox drops in the short test. Capture at 120 FPS increased
work and reduced presentation cadence in this setup, so 60 FPS was retained.
The GPU correctness self-test and synthetic 30 FPS / 4× playback test passed.
These are short local measurements with the exclusions above, not guarantees
of native Chiaki latency or 120 distinct game-motion frames per second.
