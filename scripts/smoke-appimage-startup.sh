#!/usr/bin/env bash
set -euo pipefail

"$1" > startup.log 2>&1 &
app=$!
# Moonlight can spend 30 seconds draining workers on SIGTERM. Stop only the
# process launched above because this smoke test checks startup.
trap 'kill -KILL "$app" 2>/dev/null || true; wait "$app" 2>/dev/null || true' EXIT

# The shell can still map a window when its initial StackView page fails.
if ! timeout 20s xdotool search --sync --onlyvisible --class moonlight ||
   ! kill -0 "$app" ||
   grep -qE 'QQmlApplicationEngine failed|QML StackView: push:' startup.log; then
    cat startup.log >&2
    exit 1
fi
echo 'Extracted AppImage host screen opened successfully'
