#!/usr/bin/env python3
"""Launch the local Veyra/Chiaki bridge using an existing saved registration."""
import argparse
import configparser
import ctypes as C
import json
import os
from pathlib import Path
import signal
import subprocess
import time
import uuid

parser = argparse.ArgumentParser(description=__doc__)
parser.add_argument("--host")
parser.add_argument("--nickname")
parser.add_argument("--multiplier", choices=(1, 2, 4), type=int, default=2)
parser.add_argument("--budget-ms", type=int, default=4)
args = parser.parse_args()
if not 0 <= args.budget_ms <= 100:
    parser.error("--budget-ms must be between 0 and 100.")
source = Path(__file__).resolve().parent.parent
local = Path(os.environ.get("VEYRA_LOCAL_ROOT", source.parent / "Veyra-local"))
runtime = Path(os.environ["XDG_RUNTIME_DIR"])
chiaki = local / "build/chiaki-direct/gui/chiaki"
viewer = local / "build/native/linux/player/veyra-linux"
state = runtime / "veyra-direct-session.json"

def activate(pid):
    """Request focus for our own XWayland viewer, without sending input."""
    x = C.CDLL("libX11.so.6")
    D, U = C.c_void_p, C.c_ulong
    x.XOpenDisplay.argtypes = [C.c_char_p]; x.XOpenDisplay.restype = D
    d = x.XOpenDisplay(None)
    if not d:
        return
    x.XDefaultRootWindow.argtypes = [D]; x.XDefaultRootWindow.restype = U
    root = x.XDefaultRootWindow(d)
    x.XInternAtom.argtypes = [D, C.c_char_p, C.c_int]; x.XInternAtom.restype = U
    x.XGetWindowProperty.argtypes = [D, U, U, C.c_long, C.c_long, C.c_int, U,
        C.POINTER(U), C.POINTER(C.c_int), C.POINTER(U), C.POINTER(U), C.POINTER(C.POINTER(C.c_ubyte))]
    x.XFree.argtypes = [D]
    def prop(window, name):
        typ, fmt, count, left, data = U(), C.c_int(), U(), U(), C.POINTER(C.c_ubyte)()
        x.XGetWindowProperty(d, window, x.XInternAtom(d, name, False), 0, 1024, False, 0,
            C.byref(typ), C.byref(fmt), C.byref(count), C.byref(left), C.byref(data))
        result = list(C.cast(data, C.POINTER(U))[:count.value]) if fmt.value == 32 else []
        if data:
            x.XFree(data)
        return result
    class Data(C.Union):
        _fields_ = [("b", C.c_char * 20), ("s", C.c_short * 10), ("l", C.c_long * 5)]
    class Message(C.Structure):
        _fields_ = [("type", C.c_int), ("serial", U), ("send_event", C.c_int), ("display", D),
            ("window", U), ("message_type", U), ("format", C.c_int), ("data", Data)]
    class Event(C.Union):
        _fields_ = [("message", Message), ("pad", C.c_long * 24)]
    for window in prop(root, b"_NET_CLIENT_LIST"):
        if prop(window, b"_NET_WM_PID") != [pid]:
            continue
        event = Event(); event.message.type = 33; event.message.display = d
        event.message.window = window; event.message.format = 32
        event.message.message_type = x.XInternAtom(d, b"_NET_ACTIVE_WINDOW", False)
        event.message.data.l[0] = 2
        x.XSendEvent.argtypes = [D, U, C.c_int, C.c_long, C.POINTER(Event)]
        x.XSendEvent(d, root, False, (1 << 20) | (1 << 19), C.byref(event))
        break
    x.XCloseDisplay.argtypes = [D]; x.XCloseDisplay(d)

def matches(pid, binary):
    try:
        return Path(os.readlink(f"/proc/{int(pid)}/exe").removesuffix(" (deleted)")) == binary
    except (OSError, ValueError):
        return False

if not chiaki.is_file() or not viewer.is_file():
    parser.error("Build the native viewer and run scripts/build-chiaki-direct.sh first.")
previous_viewer = None
if state.exists():
    try:
        existing = json.loads(state.read_text())
        if matches(existing["viewer"], viewer) and matches(existing["chiaki"], chiaki):
            activate(existing["viewer"])
            print("Veyra PS5 Direct is already running.")
            raise SystemExit(0)
        if matches(existing.get("viewer", 0), viewer):
            previous_viewer = existing["viewer"]
    except (ValueError, KeyError):
        pass
config = configparser.ConfigParser(interpolation=None)
config.read(Path.home() / ".config/Chiaki/Chiaki.conf")
if not args.nickname:
    if config.getint("registered_hosts", "size", fallback=0) != 1:
        parser.error("Select --nickname when more than one console is registered.")
    args.nickname = config.get("registered_hosts", "1\\server_nickname")
if not args.host:
    if config.getint("manual_hosts", "size", fallback=0) != 1:
        parser.error("Select --host when there is not exactly one saved manual host.")
    args.host = config.get("manual_hosts", "1\\host")

# Switch the existing Remote Play client, preserving its on-disk registration.
if previous_viewer is not None and matches(previous_viewer, viewer):
    os.kill(previous_viewer, signal.SIGTERM)
original = Path.home() / ".local/share/chiaki-ng/v1.10.1/squashfs-root/usr/bin/chiaki"
running = []
for entry in Path("/proc").iterdir():
    if entry.name.isdigit():
        for binary in (original, chiaki):
            if matches(entry.name, binary):
                running.append((int(entry.name), binary))
                os.kill(int(entry.name), signal.SIGTERM)
for _ in range(30):
    if not any(matches(pid, binary) for pid, binary in running):
        break
    time.sleep(0.1)
for pid, binary in running:
    if matches(pid, binary):
        # SDL builds can consume SIGTERM without closing the Qt application.
        # Recheck executable identity before replacing that exact client.
        os.kill(pid, signal.SIGKILL)
for _ in range(30):
    if not any(matches(pid, binary) for pid, binary in running):
        break
    time.sleep(0.1)
else:
    parser.error("The previous Chiaki process did not exit.")

bridge = runtime / f"veyra-{uuid.uuid4().hex}.rgba"
logs = local / "logs"
logs.mkdir(parents=True, exist_ok=True)
env = os.environ.copy()
env.update(QT_QPA_PLATFORM="xcb", QT_LINUX_ACCESSIBILITY_ALWAYS_ON="1")
with (logs / "veyra-direct.log").open("a") as output:
    v = subprocess.Popen([str(source / "scripts/run-linux.sh"), "--direct-frames", str(bridge),
        "--multiplier", str(args.multiplier), "--latency-priority", "--interpolation-budget-ms",
        str(args.budget_ms), "--fullscreen"], env=env, stdout=output, stderr=subprocess.STDOUT,
        start_new_session=True)
for _ in range(50):
    if bridge.exists():
        break
    if v.poll() is not None:
        parser.error("Viewer failed to start; inspect the local Veyra log.")
    time.sleep(0.1)
if not bridge.exists():
    v.terminate()
    parser.error("Viewer did not create its frame bridge.")
env["VEYRA_DIRECT_FRAMES"] = str(bridge)
with (logs / "chiaki-direct.log").open("a") as output:
    c = subprocess.Popen([str(chiaki), "--opengl", "stream", args.nickname, args.host],
        env=env, stdout=output, stderr=subprocess.STDOUT, start_new_session=True)
fd = os.open(state, os.O_WRONLY | os.O_CREAT | os.O_TRUNC | os.O_NOFOLLOW, 0o600)
with os.fdopen(fd, "w") as output:
    json.dump({"viewer": v.pid, "chiaki": c.pid, "bridge": str(bridge)}, output)
for _ in range(150):
    if v.poll() is not None or c.poll() is not None:
        parser.error("One of the clients exited; inspect the local direct-frame logs.")
    with bridge.open("rb") as stream:
        stream.seek(32)
        if int.from_bytes(stream.read(8), "little"):
            break
    time.sleep(0.1)
else:
    parser.error("No decoded frames arrived; inspect the Chiaki log and confirm SDR/CUDA settings.")
time.sleep(0.5)
activate(v.pid)
print("Started Veyra PS5 Direct. Audio and controller input remain in Chiaki.")
