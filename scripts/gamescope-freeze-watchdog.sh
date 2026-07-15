#!/usr/bin/env bash
# Gamescope freeze watchdog.
#
# Probes gamescope's Wayland socket with a display roundtrip.  The wlserver
# event loop that answers the roundtrip is the same loop that services KMS
# hotplug events, so a deadlocked compositor cannot answer it.  After
# consecutive probe failures this captures the kernel-side state needed to
# diagnose the deadlock (task kernel stacks, dmesg, DRM debugfs) and then
# kills gamescope so the SteamOS session wrapper relaunches Game Mode,
# avoiding a hard reboot.
#
# Must run as root: /proc/<pid>/task/*/stack and /sys/kernel/debug/dri
# are root-only.
#
# Usage:
#   gamescope-freeze-watchdog.sh                # daemon mode (systemd)
#   gamescope-freeze-watchdog.sh --capture-now  # one-shot manual capture

set -u

RUNTIME_DIR="${GAMESCOPE_WATCHDOG_RUNTIME_DIR:-/run/user/1000}"
DUMP_ROOT="${GAMESCOPE_WATCHDOG_DUMP_DIR:-/home/deck/gamescope-freeze-dumps}"
DUMP_OWNER="${GAMESCOPE_WATCHDOG_DUMP_OWNER:-deck:deck}"
PROBE_INTERVAL="${GAMESCOPE_WATCHDOG_INTERVAL:-5}"
PROBE_TIMEOUT="${GAMESCOPE_WATCHDOG_TIMEOUT:-3}"
FAILS_REQUIRED="${GAMESCOPE_WATCHDOG_FAILS:-3}"
RECOVER="${GAMESCOPE_WATCHDOG_RECOVER:-1}"
COOLDOWN="${GAMESCOPE_WATCHDOG_COOLDOWN:-120}"

log() {
    echo "$*"
}

# Round-trip one Wayland socket.  Exit codes: 0 responsive, 1 roundtrip
# failed, 2 could not connect, 124 hung until timeout.
roundtrip() {
    XDG_RUNTIME_DIR="$RUNTIME_DIR" WAYLAND_DISPLAY="$1" \
        timeout "$PROBE_TIMEOUT" python3 - <<'EOF'
import ctypes, sys
lib = ctypes.CDLL("libwayland-client.so.0")
lib.wl_display_connect.restype = ctypes.c_void_p
lib.wl_display_connect.argtypes = [ctypes.c_char_p]
lib.wl_display_roundtrip.argtypes = [ctypes.c_void_p]
lib.wl_display_disconnect.argtypes = [ctypes.c_void_p]
d = lib.wl_display_connect(None)
if not d:
    sys.exit(2)
rc = lib.wl_display_roundtrip(d)
lib.wl_display_disconnect(d)
sys.exit(0 if rc >= 0 else 1)
EOF
}

# Probe every gamescope socket.  Returns 0 if all live sockets respond,
# 1 if any is unresponsive, 2 if no gamescope session exists.
probe_gamescope() {
    local socket found=0
    for socket in "$RUNTIME_DIR"/gamescope-*; do
        [[ -S "$socket" ]] || continue
        roundtrip "$(basename "$socket")"
        case $? in
            0) found=1 ;;
            2) ;;  # stale socket from a dead gamescope; not a freeze
            *) return 1 ;;
        esac
    done
    [[ $found -eq 1 ]] && return 0
    return 2
}

capture_state() {
    local dir
    dir="$DUMP_ROOT/freeze-$(date +%Y%m%d-%H%M%S)"
    mkdir -p "$dir"

    {
        date
        uptime
        echo "kernel: $(uname -r)"
    } > "$dir/summary.txt" 2>&1

    dmesg -T | tail -n 400 > "$dir/dmesg.txt" 2>&1

    # Thread states + wait channels for everything; narrows down who is
    # stuck on what even for processes we don't stack-dump.
    ps -eLo pid,tid,ppid,state,wchan:40,pcpu,comm,args --sort pid \
        > "$dir/ps-threads.txt" 2>&1

    local pid task comm
    for pid in $(pgrep -x gamescope) $(pgrep -x Xwayland) $(pgrep -x steamcompmgr); do
        comm=$(cat "/proc/$pid/comm" 2>/dev/null) || continue
        local pdir="$dir/proc-$pid-$comm"
        mkdir -p "$pdir"
        cp "/proc/$pid/status" "$pdir/" 2>/dev/null
        for task in "/proc/$pid/task"/*; do
            {
                echo "== tid $(basename "$task") ($(cat "$task/comm" 2>/dev/null))" \
                     "state=$(awk '/^State:/{print $2}' "$task/status" 2>/dev/null)" \
                     "wchan=$(cat "$task/wchan" 2>/dev/null)"
                cat "$task/stack" 2>/dev/null || echo "(no kernel stack)"
                echo
            } >> "$pdir/kernel-stacks.txt"
        done
    done

    # DRM debugfs reads take DRM locks; a deadlock holding them would hang
    # the read, so every one gets its own timeout.
    local dri f
    for dri in /sys/kernel/debug/dri/*; do
        [[ -d "$dri" ]] || continue
        for f in state clients framebuffer amdgpu_fence_info amdgpu_gem_info; do
            [[ -e "$dri/$f" ]] || continue
            timeout 5 cat "$dri/$f" > "$dir/dri-$(basename "$dri")-$f.txt" 2>&1
        done
    done

    chown -R "$DUMP_OWNER" "$DUMP_ROOT" 2>/dev/null
    log "captured freeze state to $dir"
}

recover_session() {
    # SIGKILL because a deadlocked compositor will not service SIGTERM.
    # The SteamOS session wrapper relaunches Game Mode when gamescope dies.
    log "killing gamescope to restart the session"
    pkill -9 -x gamescope || true
}

if [[ "${1:-}" == "--capture-now" ]]; then
    capture_state
    exit 0
fi

mkdir -p "$DUMP_ROOT"
log "watchdog started: interval=${PROBE_INTERVAL}s timeout=${PROBE_TIMEOUT}s" \
    "fails=${FAILS_REQUIRED} recover=${RECOVER}"

fails=0
last_action=0
while true; do
    sleep "$PROBE_INTERVAL"

    probe_gamescope
    case $? in
        0|2)
            fails=0
            continue
            ;;
    esac

    fails=$((fails + 1))
    log "gamescope probe failed ($fails/$FAILS_REQUIRED)"
    [[ $fails -lt $FAILS_REQUIRED ]] && continue

    fails=0
    now=$(date +%s)
    if (( now - last_action < COOLDOWN )); then
        log "in cooldown; skipping action"
        continue
    fi
    last_action=$now

    log "gamescope declared frozen"
    capture_state
    if [[ "$RECOVER" == 1 ]]; then
        recover_session
    fi
done
