# Agent Notes

- This Moonlight dev checkout is built inside the `moonlight-dev` Distrobox, not with the host toolchain. The host may not have `g++`, `make`, or other build tools in `PATH`.
- The dev binary launched by the desktop shortcuts is `/home/deck/sources/moonlight-qt/build/app/moonlight`.
- The Desktop launcher `Moonlight Dev (Logged)` runs `/home/deck/.local/bin/moonlight-dev-log`, which logs to `~/moonlight-logs/latest.log` and then calls `/home/deck/.local/bin/moonlight-dev`.
- The `moonlight-dev` wrapper enters the `moonlight-dev` Distrobox and runs the built binary with the container's Qt/FFmpeg stack. It chooses Wayland/X11 variables based on the current session without forcing a renderer or session type.
- For build or compile validation, enter the Distrobox first, for example: `distrobox enter --name moonlight-dev -- <build command>`.
- For real Steam Deck presentation testing, use Steam/Game Mode. Desktop launches are useful for logs and quick checks, but nested gamescope from Plasma Desktop is only a fallback and KWin still owns the physical display.
