#!/usr/bin/env bash
set -Eeuo pipefail

APP_ID="com.moonlight_stream.Moonlight"
TAG="v6.1.0-vrr8"
BUNDLE_NAME="Moonlight-VRR-6.1.0-vrr8-linux-x86_64.flatpak"
BUNDLE_URL="https://github.com/Nonary/moonlight-qt/releases/download/${TAG}/${BUNDLE_NAME}"
BUNDLE_SHA256="e17da56ce0da97921a351d4bfdbb78bcbbc20ab472797508228245bb69997df5"

MESA_REF="org.freedesktop.Platform.GL.default//25.08"
MESA_COMMIT="48bdc2284c98930fde577ec784fd9ad08bde585877c0e60c1acd7374b36c3496"
MESA_EXTRA_REF="org.freedesktop.Platform.GL.default//25.08-extra"
MESA_EXTRA_COMMIT="600b80a19d11d98602951a69b334deda1491d1057d2054dc04d0d7786f33e544"

UNIT_NAME="moonlight-vrr8-compatibility-reset"
USER_UNIT_DIR="${XDG_CONFIG_HOME:-$HOME/.config}/systemd/user"
USER_LIBEXEC_DIR="${XDG_DATA_HOME:-$HOME/.local/share}/moonlight-vrr8"
UNFREEZE_SCRIPT="$USER_LIBEXEC_DIR/reset-compatibility-fix.sh"
SERVICE_FILE="$USER_UNIT_DIR/$UNIT_NAME.service"
TIMER_FILE="$USER_UNIT_DIR/$UNIT_NAME.timer"

info() {
    printf '\n==> %s\n' "$*"
}

die() {
    printf '\nERROR: %s\n' "$*" >&2
    exit 1
}

if [ "$(id -u)" -eq 0 ]; then
    die "Run this script as the normal Steam Deck user, not with sudo."
fi

case "${XDG_SESSION_DESKTOP:-}:${DESKTOP_SESSION:-}:${SteamDeck:-}" in
    *gamescope*|*:1)
        die "Switch to Desktop Mode before running this script."
        ;;
esac

for command_name in flatpak curl sha256sum systemctl date; do
    command -v "$command_name" >/dev/null 2>&1 || die "Required command not found: $command_name"
done

cat <<'EOF'

Moonlight vrr8 SteamOS installer
--------------------------------

This script will:
  1. Download and install Moonlight vrr8 for your user account.
  2. Apply the temporary SteamOS Gaming Mode compatibility fix.
  3. Automatically remove the temporary fix after 14 days.

Moonlight must be closed while this runs.
EOF

read -r -p $'\nContinue? [y/N] ' answer
case "$answer" in
    y|Y|yes|YES|Yes) ;;
    *) echo "Cancelled."; exit 0 ;;
esac

work_dir="$(mktemp -d)"
trap 'rm -rf "$work_dir"' EXIT
bundle_path="$work_dir/$BUNDLE_NAME"

info "Downloading Moonlight vrr8"
curl --fail --location --retry 3 --output "$bundle_path" "$BUNDLE_URL"

info "Verifying the downloaded bundle"
printf '%s  %s\n' "$BUNDLE_SHA256" "$bundle_path" | sha256sum --check --status || \
    die "The downloaded Flatpak checksum did not match. Nothing was installed."

info "Installing Moonlight vrr8 on the user master branch"
flatpak install --user --noninteractive --reinstall "$bundle_path"

info "Applying the SteamOS Gaming Mode compatibility fix"
flatpak update --user --noninteractive --commit="$MESA_COMMIT" "$MESA_REF"
flatpak update --user --noninteractive --commit="$MESA_EXTRA_COMMIT" "$MESA_EXTRA_REF"

info "Temporarily freezing the working graphics components"
flatpak mask --user "$MESA_REF"
flatpak mask --user "$MESA_EXTRA_REF"

mkdir -p "$USER_UNIT_DIR" "$USER_LIBEXEC_DIR"

cat >"$UNFREEZE_SCRIPT" <<EOF
#!/usr/bin/env bash
set -u

flatpak mask --user --remove "$MESA_REF" || true
flatpak mask --user --remove "$MESA_EXTRA_REF" || true
flatpak update --user --noninteractive "$MESA_REF" "$MESA_EXTRA_REF" || true

systemctl --user disable --now "$UNIT_NAME.timer" >/dev/null 2>&1 || true
rm -f "$SERVICE_FILE" "$TIMER_FILE" "$UNFREEZE_SCRIPT"
systemctl --user daemon-reload || true
EOF
chmod 755 "$UNFREEZE_SCRIPT"

cat >"$SERVICE_FILE" <<EOF
[Unit]
Description=Remove the temporary Moonlight vrr8 compatibility fix

[Service]
Type=oneshot
ExecStart=$UNFREEZE_SCRIPT
EOF

unfreeze_at="$(date --date='+14 days' '+%Y-%m-%d %H:%M:%S')"
cat >"$TIMER_FILE" <<EOF
[Unit]
Description=Remove the Moonlight vrr8 compatibility fix after 14 days

[Timer]
OnCalendar=$unfreeze_at
Persistent=true
AccuracySec=15m
Unit=$UNIT_NAME.service

[Install]
WantedBy=timers.target
EOF

info "Scheduling automatic removal for $unfreeze_at"
systemctl --user daemon-reload
systemctl --user enable --now "$UNIT_NAME.timer"

flatpak info --user "$APP_ID//master" >/dev/null

cat <<EOF

Installation complete.

Moonlight vrr8 is installed and the temporary SteamOS compatibility fix is active.
The temporary freeze will be removed automatically on $unfreeze_at.

Next steps:
  1. Open Steam in Desktop Mode.
  2. Choose Games > Add a Non-Steam Game to My Library.
  3. Select Moonlight. If an older Moonlight shortcut already exists,
     remove it first so Steam picks up the user master branch.
  4. Return to Gaming Mode and launch Moonlight.

The temporary freeze can also delay graphics-component updates for some
other user-installed Flatpaks. It will be removed automatically after 14 days.
EOF
