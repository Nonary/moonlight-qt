# SteamOS VRR and the performance overlay

Investigated 2026-09-09 against Moonlight `faeff9bd` plus the existing local
VRR work, Gamescope `3.16.10`, and upstream Gamescope
`b385948cce5858e69d18e48c43c6baabdf258b85`. An affected SteamOS device has not
yet been tested with the candidate. This is a source-backed fix candidate
and a reversible workaround procedure, not confirmation of the reported cause.

## Candidate in Moonlight

Linux VRR prefers Vulkan even for 8-bit SDR. Previously, Gamescope surfaces
without Immediate support went straight to the WSI FIFO compatibility path;
Mailbox was never queried. The selector now tries Immediate, then Mailbox,
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
