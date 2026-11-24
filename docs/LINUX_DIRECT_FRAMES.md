# Direct decoded frames from Chiaki

This optional Linux path passes SDR decoded video from a locally patched
chiaki-ng to Veyra. It avoids displaying, scaling and recapturing a window.
Chiaki still owns the console session, audio, controller input and registration.
No credentials are passed to Veyra or stored in this repository.

## Build

Build the native Veyra player first as described in `LINUX_PLAYER.md`. The
additional build needs Git, CMake, Ninja, uv, a C/C++ compiler, Qt 6 with its
private GUI headers, QML/Quick/SVG/Widgets, FFmpeg development headers/libraries,
libplacebo >= 7.349, SDL2, Opus, OpenSSL, json-c, miniupnpc, and curl with WebSocket
support. The helper downloads source and a local Python environment into the
sibling Veyra-local directory; it does not install system packages or drivers.

```sh
bash scripts/build-chiaki-direct.sh
```

The helper pins chiaki-ng v1.10.1 to
`f30da0aaeb96fc0026cc03683153f682352e7d7c` and applies
`linux/chiaki/chiaki-ng-v1.10.1.patch`. Its submodule revisions are pinned by that
commit. Vulkan-Headers v1.3.290 is pinned to
`b379292b2ab6df5771ba9870d53cf8b2c9295daf`; these are build headers, not a driver
replacement. The private Python 3.12 environment uses protobuf 7.36.2.

## Run

Pair the console in chiaki-ng and verify ordinary Remote Play first. Select
CUDA decoding and H.265 SDR with the OpenGL renderer for the tested path.
Then launch:

```sh
python scripts/run-ps5-direct.py
```

With exactly one registered console and one manual host, the launcher reads the
saved nickname and host locally. Otherwise specify `--nickname` and `--host`.
It closes an existing supported Chiaki client before starting the patched one;
it does not create a second simultaneous PS5 stream. An already-running direct
session is brought to the front. The normal AppImage remains installed.
If that client ignores the shutdown signal, the launcher force-closes the
verified Chiaki process after three seconds before reconnecting.

The default is 2× generation with latency priority and a **4 ms interpolation
waiting budget**. This is a responsiveness setting: generated frames that miss
their deadlines are skipped, and motion will not be uniformly 120 FPS. To
remove the cap use `--budget-ms 0`; to bypass generation use `--multiplier 1`.
Neither eliminates PS5 rendering/encoding, network, decoding or display delay.
GTA VI's 30 FPS rendering target is not changed by this application.

Audio continues directly from Chiaki, without automatic delay compensation.
Chiaki postprocessing (gamma, sharpening, display color management) is not
included in this pre-render feed. The bridge converts SDR YUV using its range
and matrix metadata to full-range RGBA8. HDR/wide-gamut input is rejected.

## Transport and validation

The viewer creates a unique mode-0600 file in XDG_RUNTIME_DIR. The sender opens
that same file only when VEYRA_DIRECT_FRAMES explicitly names it. It contains
one bounded frame slot, dimensions, source PTS and a monotonic timestamp; it
contains no pairing material. A nonblocking advisory lock protects conversion
and copying. A busy reader makes the writer drop a frame rather than wait or
queue. The viewer polls the latest slot at 1 ms intervals. CPU copies remain;
this is not GPU-to-GPU zero-copy transport.

The sender notices a replaced receiver file and reconnects. Normal viewer exit
removes its own file; an abrupt termination can leave the private runtime file
until logout. No sockets or network listeners are opened by this bridge.

Run its transport/color tests with:

```sh
c++ -std=c++17 -O2 -Wall -Wextra linux/chiaki/bridge-test.cpp \
  -o ../Veyra-local/tests/bridge-test $(pkg-config --cflags --libs libavutil libswscale)
timeout 30 ../Veyra-local/tests/bridge-test
```

Tests cover interprocess sharing, writer contention, limited-range black/white
conversion, HDR rejection, receiver restart and exclusive file creation.
The live PS5/CachyOS/RTX 5080 test verified decoded frames and active NVIDIA
generation. With the 4 ms budget, a short run measured approximately 4–6 ms
from decoder delivery (before the CPU transfer) to the viewer's Qt swap. This
is not physical display latency or controller-to-screen latency. Estimated
content age varies because interpolated frames describe earlier moments.

The previous window-capture measurements started after window capture; the
direct-path timestamp starts earlier, so these numbers are not a simple
apples-to-apples total latency comparison. The removed window sampling and
Chiaki display waits were outside the earlier measurement.

## Source and licensing

chiaki-ng is by its authors and contributors at
https://github.com/chiaki-ng/chiaki-ng, under AGPLv3 with its stated OpenSSL
exception. Its complete source and license remain in the pinned checkout.
The patch adds an optional decoded-frame callback and libswscale linkage.
Veyra's bridge headers and tests are GPL-3.0-or-later; the modified Chiaki
executable is subject to Chiaki's combined-work AGPL requirements. No modified
Chiaki binary is distributed in this repository. Keep the pinned source,
patch, included bridge headers and original notices together when distributing
a build. Vulkan-Headers uses its upstream Apache-2.0/MIT notices.
