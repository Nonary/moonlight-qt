#!/usr/bin/env bash
# Launch the locally built Moonlight client from Steam/Game Mode.
#
# The moonlight-dev container must be started by the user systemd unit
# scripts/moonlight-dev-container.service before this script is invoked.  A
# Steam shortcut must not own the container itself: Steam's process cleanup
# otherwise kills the container (and any other service using it) when the
# shortcut stops.

set -euo pipefail

CONTAINER_NAME="${MOONLIGHT_DEV_CONTAINER:-moonlight-dev}"
MOONLIGHT_BIN="${MOONLIGHT_DEV_BIN:-/home/deck/sources/moonlight-qt/build/app/moonlight}"
GAMESCOPE_WSI_LAYER_DIR="${MOONLIGHT_GAMESCOPE_WSI_LAYER_DIR:-/home/deck/sources/moonlight-qt/packaging/flatpak}"

if [[ ! -x "$MOONLIGHT_BIN" ]]; then
    echo "moonlight-dev: native binary not found: $MOONLIGHT_BIN" >&2
    exit 127
fi

if ! command -v podman >/dev/null 2>&1; then
    echo "moonlight-dev: podman is required on the host" >&2
    exit 127
fi

container_running() {
    [[ "$(podman inspect --format '{{.State.Running}}' "$CONTAINER_NAME" 2>/dev/null || true)" == true ]]
}

# Start the container through systemd, outside Steam's game process scope.
# This is intentionally not `podman start` here: starting it from the Steam
# shortcut makes Steam responsible for the container's lifetime.
if ! container_running; then
    # This unit is a RemainAfterExit oneshot.  `start` is deliberately a
    # no-op while the unit is still active even if a previous Game Mode
    # transition stopped the container, so use restart to rerun ExecStart.
    if ! systemctl --user restart moonlight-dev-container.service; then
        echo "moonlight-dev: cannot start moonlight-dev-container.service" >&2
        echo "Install scripts/moonlight-dev-container.service and enable it first." >&2
        exit 125
    fi
fi

if ! container_running; then
    echo "moonlight-dev: container '$CONTAINER_NAME' is not running" >&2
    exit 125
fi

in_gamescope_session() {
    if [[ "${XDG_SESSION_DESKTOP:-}" == gamescope ]] ||
        [[ "${DESKTOP_SESSION:-}" == gamescope ]] ||
        [[ "${SteamDeck:-}" == 1 ]]; then
        return 0
    fi
    command -v gamescope-type >/dev/null 2>&1 && gamescope-type >/dev/null 2>&1
}

podman_args=(
    exec
    --user deck
    --workdir /home/deck/sources/moonlight-qt
)

# Keep only display/session values that are needed by the client.  In
# particular, do not forward Steam's LD_PRELOAD/AppId/pressure-vessel values.
# SteamVirtualGamepadInfo and ALLOW_STEAM_VIRTUAL_GAMEPAD must accompany
# Steam's SDL ignore list or SDL filters out the virtual pads with the physical
# controllers, leaving Moonlight with no controller devices in Game Mode.
for name in \
    DISPLAY \
    XAUTHORITY \
    XDG_RUNTIME_DIR \
    DBUS_SESSION_BUS_ADDRESS \
    PULSE_SERVER \
    LIBVA_DRIVER_NAME \
    PREFER_VULKAN \
    DRI_PRIME \
    MESA_VK_DEVICE_SELECT \
    VK_ICD_FILENAMES \
    VK_LAYER_PATH \
    MOONLIGHT_VRR_TRACE \
    SDL_GAMECONTROLLERCONFIG \
    SDL_GAMECONTROLLER_ALLOW_STEAM_VIRTUAL_GAMEPAD \
    SDL_GAMECONTROLLER_IGNORE_DEVICES \
    SDL_GAMECONTROLLER_IGNORE_DEVICES_EXCEPT \
    SteamVirtualGamepadInfo \
    SteamVirtualGamepadInfo_Proton; do
    if [[ -n "${!name:-}" ]]; then
        podman_args+=(--env "${name}=${!name}")
    fi
done

if in_gamescope_session; then
    if [[ -z "${DISPLAY:-}" ]]; then
        echo "moonlight-dev: Gamescope session has no DISPLAY" >&2
        exit 125
    fi
    podman_args+=(
        --env SDL_AUDIODRIVER=pulseaudio
        --env SDL_VIDEODRIVER=x11
        --env QT_QPA_PLATFORM=xcb
        --env WAYLAND_DISPLAY=
        --env XDG_SESSION_TYPE=x11
        --env XDG_SESSION_DESKTOP=gamescope
        --env DESKTOP_SESSION=gamescope-wayland
    )

    # The native Distrobox image does not ship Gamescope's implicit Vulkan
    # WSI layer. Loading the host layer lets our Vulkan swapchain bypass
    # XWayland, which is required for Gamescope to expose HDR surface formats.
    # The layer JSON uses /var/run/host, which Distrobox mounts from the host.
    if [[ -r "$GAMESCOPE_WSI_LAYER_DIR/VkLayer_FROG_gamescope_wsi.x86_64.json" ]]; then
        podman_args+=(
            --env ENABLE_GAMESCOPE_WSI=1
            --env "VK_ADD_IMPLICIT_LAYER_PATH=$GAMESCOPE_WSI_LAYER_DIR"
        )
    else
        echo "moonlight-dev: Gamescope Vulkan WSI layer is unavailable; HDR presentation will not work in Game Mode" >&2
    fi
elif [[ -n "${WAYLAND_DISPLAY:-}" ]]; then
    podman_args+=(
        --env SDL_AUDIODRIVER=pulseaudio
        --env SDL_VIDEODRIVER=wayland,x11
        --env QT_QPA_PLATFORM='wayland;xcb'
        --env XDG_SESSION_TYPE=wayland
    )
else
    podman_args+=(
        --env SDL_AUDIODRIVER=pulseaudio
        --env SDL_VIDEODRIVER=x11
        --env QT_QPA_PLATFORM=xcb
        --env XDG_SESSION_TYPE=x11
    )
fi

podman_args+=("$CONTAINER_NAME" "$MOONLIGHT_BIN")
podman_args+=("$@")

# `env -u` applies to the host-side podman client as well as the command
# environment.  This avoids Steam's overlay preload and compatibility runtime
# being copied into the Distrobox process tree.
exec env \
    -u LD_PRELOAD \
    -u LD_LIBRARY_PATH \
    -u SteamAppId \
    -u SteamGameId \
    -u SteamOverlayGameId \
    -u STEAM_COMPAT_APP_ID \
    -u STEAM_COMPAT_DATA_PATH \
    -u STEAM_COMPAT_CLIENT_INSTALL_PATH \
    -u STEAM_RUNTIME \
    -u STEAM_RUNTIME_HEAVY \
    -u PRESSURE_VESSEL_APP_LD_LIBRARY_PATH \
    -u PRESSURE_VESSEL_FILESYSTEMS_RO \
    podman "${podman_args[@]}"
