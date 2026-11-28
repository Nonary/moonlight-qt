#!/bin/sh
set -eu

if [ "$(id -u)" != 0 ]; then
    echo "Run this installer with sudo in Steam Deck Desktop Mode." >&2
    exit 1
fi
id deck >/dev/null
src=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
install -d -m 0755 /etc/benq-gr10-vrr
install -m 0755 "$src/gr10-vrr-reset" /etc/benq-gr10-vrr/gr10-vrr-reset
install -m 0644 "$src/gr10-preflight.py" /etc/benq-gr10-vrr/gr10-preflight.py
install -m 0644 "$src/benq-gr10-vrr.socket" "$src/benq-gr10-vrr@.service" /etc/systemd/system/
systemctl daemon-reload
systemctl enable --now benq-gr10-vrr.socket
