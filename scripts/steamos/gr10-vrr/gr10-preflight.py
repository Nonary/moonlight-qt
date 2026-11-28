#!/usr/bin/python3 -I
"""Record the Deck's HDMI bridge state and handle Moonlight's pre-stream request."""
import fcntl
import hashlib
import json
import subprocess
import sys
import time
from pathlib import Path

CONNECTOR = Path("/sys/class/drm/card0-DP-1")
DEBUG = Path("/sys/kernel/debug/dri/0")
BROKEN = "11bcc6aec274a455e4a9d82a310fda84fa528106907b46df1af3c7f195242e50"
HEALTHY = "180ce68403d07982a788586cf51bf377d46bb858efa328a1926293361e7672e2"
STATE = Path("/run/benq-gr10-vrr")
RESET = "/etc/benq-gr10-vrr/gr10-vrr-reset"


def read(path):
    try:
        return path.read_text().strip()
    except OSError:
        return ""


def edid_digest():
    try:
        edid = (CONNECTOR / "edid").read_bytes()
    except OSError:
        edid = b""
    return hashlib.sha256(edid).hexdigest() if edid else ""


def drm_clients():
    # amdgpu debugfs prints "command pid dev master ...". Comm may contain spaces.
    entries = read(DEBUG / "clients").splitlines()
    result = []
    for line in entries[1:]:
        fields = line.split()
        for index in range(1, len(fields) - 2):
            if fields[index].isdigit() and fields[index + 1] == "0":
                name = " ".join(fields[:index])
                master = fields[index + 2] == "y"
                if master or name in ("kwin_wayland", "gamescope", "moonlight", "tsukuyomi", "modetest"):
                    result.append((name, fields[index], master))
                break
    return sorted(set(result))


def aux_digest():
    # Read the two blocks previously captured from this LG over DP AUX.
    # Discover the adapter dynamically; the connector's "ddc" link is a different bus.
    for adapter in CONNECTOR.glob("i2c-*"):
        if "aux" not in read(adapter / "name").lower():
            continue
        try:
            data = subprocess.check_output(
                ["/usr/bin/i2ctransfer", "-b", "-y", adapter.name[4:],
                 "w1@0x50", "0", "r128", "w1@0x50", "128", "r128"],
                stderr=subprocess.DEVNULL, timeout=1)
        except (OSError, subprocess.SubprocessError):
            return "read-unavailable"
        return hashlib.sha256(data).hexdigest() if len(data) == 256 else "short-read"
    return "no-aux-adapter"


def trace():
    previous = None
    follow_up = None
    while True:
        state = {"vt": read(Path("/sys/class/tty/tty0/active")), "drm_clients": drm_clients(),
                 "connector": read(CONNECTOR / "status"), "enabled": read(CONNECTOR / "enabled"),
                 "edid": edid_digest(), "vrr_range": read(DEBUG / "DP-1/vrr_range")}
        if state != previous:
            event = {"monotonic": time.monotonic(), **state}
            if state["edid"] in (BROKEN, HEALTHY):
                event["aux_edid"] = aux_digest()
                follow_up = time.monotonic() + 2
            else:
                follow_up = None
            print(json.dumps(event), flush=True)
            previous = state
        elif follow_up is not None and time.monotonic() >= follow_up:
            print(json.dumps({"monotonic": time.monotonic(), **state,
                              "aux_edid": aux_digest(), "sample": "settled"}), flush=True)
            follow_up = None
        time.sleep(0.25 if state["edid"] else 1.0)


def ethernet_default():
    data = subprocess.check_output(["/usr/bin/ip", "-j", "route", "show", "default"], timeout=3)
    routes = sorted(json.loads(data), key=lambda r: r.get("metric", 0))
    if routes:
        iface = routes[0].get("dev", "")
        if iface and "/usb" in str((Path("/sys/class/net") / iface / "device").resolve()):
            return iface
    return ""


def vrr_restored():
    limits = read(DEBUG / "DP-1/vrr_range").splitlines()
    return edid_digest() == HEALTHY and limits == ["Min: 40", "Max: 120"]


def preflight():
    digest = edid_digest()
    if digest == HEALTHY:
        return "ok" if vrr_restored() else "unavailable"
    if digest != BROKEN:
        return "unavailable"
    with (STATE / "recovery.lock").open("a+") as lock:
        fcntl.flock(lock, fcntl.LOCK_EX | fcntl.LOCK_NB)
        time.sleep(0.6)
        digest = edid_digest()
        if digest != BROKEN:
            return "ok" if vrr_restored() else "unavailable"
        last = STATE / "last-attempt"
        if last.exists() and time.monotonic() - float(read(last)) < 60:
            return "cooldown"
        interface = ethernet_default()
        last.write_text(str(time.monotonic()))
        print(f"Recovery requested for DP-1, original dock route {interface!r}", file=sys.stderr, flush=True)
        deadline = time.monotonic() + 65
        subprocess.run([RESET], stdout=sys.stderr, stderr=sys.stderr, check=True, timeout=40)
        while time.monotonic() < deadline:
            if vrr_restored() and (not interface or ethernet_default() == interface):
                print("Verified original VRR EDID and network route", file=sys.stderr, flush=True)
                return "ok"
            time.sleep(0.5)
        return "network" if vrr_restored() else "unavailable"


if __name__ == "__main__":
    if sys.argv[1:] == ["trace"]:
        trace()
    elif sys.argv[1:] == ["preflight"] and sys.stdin.buffer.readline(24) == b"preflight DP-1\n":
        try:
            result = preflight()
        except (OSError, ValueError, subprocess.SubprocessError) as error:
            print(f"Recovery failed: {error}", file=sys.stderr, flush=True)
            result = "failed"
        print(result, flush=True)
    else:
        sys.exit(2)
