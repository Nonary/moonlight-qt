> The forced-composition and per-frame repaint checkboxes described historically
> below are retired. Saved values no longer activate them. Use **Oscillate VRR
> latency** for the current within-stream preset comparison; see
> [the capture procedure](vrr-latency-captures.md).

# SteamOS VRR and the performance overlay

Investigated 2026-09-09 against Moonlight `faeff9bd` plus the existing local
VRR work, Gamescope `3.16.10`, and upstream Gamescope
`b385948cce5858e69d18e48c43c6baabdf258b85`. Updated 2026-09-10 after testing
on Gamescope 3.16.23.5: the application mode changed from FIFO to Mailbox, but
the user still needed Steam's performance overlay for smooth motion. Mailbox
alone did not resolve the symptom. The checkbox now tests forced composition
instead; the saved Mailbox preference no longer affects production. See
[capture findings](steamos-judder-20260910.md).

## Previous Mailbox experiment

Linux VRR prefers Vulkan even for 8-bit SDR. Previously, Gamescope surfaces
without Immediate support went straight to the WSI FIFO compatibility path;
Mailbox was never queried. The retired experiment made the selector try Immediate, then Mailbox,
and retains FIFO only for the existing Gamescope WSI exception when neither
adaptive mode is exposed. Ordinary Wayland and X11/KMSDRM selection is unchanged.

Gamescope's WSI layer uses an underlying Mailbox swapchain but communicates the
application's original mode separately. Gamescope identifies FIFO commits from
that original mode and applies its own commit scheduling. Thus requesting FIFO
is not equivalent to requesting Mailbox even though both use Mailbox underneath.
The change avoids that extra FIFO policy when the surface supports Mailbox.

Source evidence:

- [WSI mode enumeration](https://github.com/ValveSoftware/gamescope/blob/3.16.10/layer/VkLayer_FROG_gamescope_wsi.cpp#L912-L928): modes normally come from the driver; a FIFO-only list is conditional.
- [WSI original-mode forwarding](https://github.com/ValveSoftware/gamescope/blob/3.16.10/layer/VkLayer_FROG_gamescope_wsi.cpp#L1384-L1396): Steam's limiter may also force FIFO.
- [FIFO surface classification](https://github.com/ValveSoftware/gamescope/blob/3.16.10/src/wlserver.cpp#L2561-L2575) and [commit scheduling](https://github.com/ValveSoftware/gamescope/blob/3.16.10/src/steamcompmgr.cpp#L6421-L6489).

The session log now reports `Gamescope VRR selected Mailbox application
presentation` (or Immediate/FIFO), with WSI *requested* state. This is the mode
Moonlight selected, not proof that the layer loaded, that a limiter respected
the request, or that every submitted frame reached physical scanout. Preserve
the adjacent `[Gamescope WSI]` initialization messages when collecting logs.

## Workaround to try with the existing build

The reported workaround is enabling Steam's performance overlay. A closely
matching [SteamOS report](https://github.com/ValveSoftware/SteamOS/issues/2023)
also involved 10-bit Vulkan output. Turning off HDR alone is not a reliable
renderer comparison in this fork because VRR still selects Vulkan.

To test an invisible alternative, force composition in the **existing gaming
session**, with the performance overlay off. Run these as the logged-in gaming
user from a terminal or SSH connection to that session, outside the Moonlight
Flatpak. Keep the display refresh and Moonlight settings the same, and disable
Steam's per-game frame limiter so it cannot add FIFO throttling.

First inspect the live compositor and record the original value:

```sh
gamescopectl version
gamescopectl help
gamescopectl composite_force
```

Require `composite_force` in the help output. Then enable it and read it back:

```sh
gamescopectl composite_force 1
gamescopectl composite_force
```

If the original value was false/0, restore it after the comparison:

```sh
gamescopectl composite_force 0
```

If it was already true, preserve that value: composition was already forced,
so this is not a new test condition. Re-query after launching the stream in
case another component changes it. Check the returned text as well as the exit
code; some versions acknowledge an unknown command without a failing exit code.

`gamescopectl` connects using `GAMESCOPE_WAYLAND_DISPLAY`, defaulting to
`gamescope-0`. A connection failure means it did not change the compositor.
An SSH session needs the gaming user's actual `XDG_RUNTIME_DIR` and Gamescope
socket, not a guessed display from desktop mode. Do not start nested Gamescope
to apply this setting: that tests a different compositor path.

This setting affects the whole current Gamescope session. It disables direct
scanout and adds GPU composition, which can cost power or latency. It does not
itself turn off adaptive sync or change the display refresh. Keep it temporary
until the affected device shows both smooth motion and retained VRR.

The control is defined in [Gamescope](https://github.com/ValveSoftware/gamescope/blob/b385948cce5858e69d18e48c43c6baabdf258b85/src/steamcompmgr.cpp)
as `composite_force`; the [DRM backend](https://github.com/ValveSoftware/gamescope/blob/b385948cce5858e69d18e48c43c6baabdf258b85/src/Backends/DRMBackend.cpp#L3668)
uses it to require full composition. The [control utility](https://github.com/ValveSoftware/gamescope/blob/b385948cce5858e69d18e48c43c6baabdf258b85/src/Apps/gamescopectl.cpp#L77-L88)
documents the socket selection in code. Availability depends on the installed
Gamescope build.

## Checkbox A/B test

On Linux, Settings now offers **Test forced composition in Gaming Mode** in
place of the Mailbox experiment. It defaults off and persists between launches.
The old Mailbox setting is ignored. No Steam Launch Options changes are needed.

- Unchecked (A): leave Gamescope composition policy unchanged.
- Checked (B): read and force composition for the stream, verify it, and restore
  the previous setting on exit. An already-forced setting is preserved.

Reconnect after changing it. Compare the same moving scene with Steam's
performance overlay off and the same limiter settings. The log reports successful
verification and restoration. If setup cannot be verified, Moonlight displays a
launch error; uncheck the option to stream normally. Desktop Mode is unaffected.
This experiment may increase GPU power use and has not yet been validated as a
remedy for the reported motion. Setup/teardown commands never run on the pacing
worker. Other software can still change the compositor policy during a stream;
use the optional monitored launcher below when investigating that possibility.

## Determine which remedy works

Use the same moving scene and source cadence for each comparison. Record device,
SteamOS and Gamescope versions, Moonlight build, package format, display refresh,
codec/bit depth, Steam limiter state, selected application mode, and visible
result. Compare the existing build with overlay off/on, then forced composition
with overlay off. Restore the compositor setting before testing the candidate.

- If forced composition fixes it, it is a usable temporary workaround and
  implicates a difference between direct scanout and composition. It does not
  identify the faulty driver or compositor function by itself.
- If the candidate selects Mailbox and fixes it without the overlay or forced
  composition, the application's FIFO presentation path is implicated.
- If the candidate logs FIFO, the adaptive modes were unavailable; this change
  did not alter that session's mode. If it logs Immediate on both builds, this
  candidate likewise did not change the selection.
- If only the performance overlay fixes it, investigate the installed version's
  overlay repaint logic and device behavior. Overlay scheduling changed between
  Gamescope versions; an overlay can affect both scheduling and composition.

Do not implement dummy frame repetition or increase Moonlight's playout buffer
from this symptom alone. Vulkan submission statistics do not establish physical
frame delivery. Use display feedback or an external recording when determining
whether the candidate displays all frames, and measure latency separately.

## Optional monitored composition test launcher

`scripts/moonlight-gamescope-composition-test.py` runs the composition test in
the existing Gaming Mode session, outside Distrobox. It requires a supplied
`GAMESCOPE_WAYLAND_DISPLAY`, verifies support and the current value, enables
composition, and reads the value back. It rechecks three seconds after launch
and every ten seconds thereafter, logging any loss of the test condition.
It restores the original value when the command exits, including failed launches
and nonzero exits. An already-enabled value is preserved and explicitly reported
as an unchanged test condition. SIGKILL or a system crash cannot run cleanup;
the recovery command is `gamescopectl composite_force 0` if the prior value was 0.

For the existing Moonlight Steam shortcut, temporarily use this Launch Options
line (the script must be executable):

```text
/home/deck/sources/moonlight-qt/scripts/moonlight-gamescope-composition-test.py %command%
```

Leave the performance overlay off and keep the same stream settings and scene.
Remove the Launch Options line for the ordinary comparison. The local convenience
command `~/.local/bin/moonlight-dev-composite` runs the same test with
`~/.local/bin/moonlight-dev` by default. Logs go to
`~/moonlight-logs/composition-test-<time>-<pid>.log` beside the ordinary app logs.
`--check` only verifies control support and reads the current state. A Desktop
Mode refusal is expected; it does not validate the Gaming Mode workaround.

The helper changes the current compositor's global composition setting only
for the run. Extra GPU composition can affect power and latency. It does not
simulate the overlay's repaint scheduling, so failure here leaves that other
overlay effect unresolved. The installed-version control and composition branch
are defined in [Gamescope 3.16.23.5](https://github.com/ValveSoftware/gamescope/blob/3.16.23.5/src/Backends/DRMBackend.cpp#L3496-L3497).
