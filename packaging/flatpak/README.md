# Flatpak packaging (SteamOS / Steam Deck)

This directory builds the VRR-pacing fork of Moonlight as a Flatpak, which is
the supported way to run it on SteamOS: the read-only root filesystem rules out
native packages, and binaries built on a rolling distro won't match SteamOS's
glibc. The manifest is derived from the official Flathub packaging
(`flathub/com.moonlight_stream.Moonlight`) with these changes:

- The `moonlight` module builds this fork instead of upstream `v6.1.0`
  (the Qt 6.9 UI patches from Flathub are already merged here).
- `--env=PREFER_VULKAN=1`: the VRR cadence pacing lives in the Vulkan
  (libplacebo) renderer, so the Flatpak selects it by default. Launch with
  `PREFER_VULKAN=0` to get the stock renderer order.
- `CONFIG+=disable-libdrm` removed: the fork reads the true display refresh
  rate from DRM scanout state (gamescope's XWayland advertises a fake 60 Hz
  mode), which needs `HAVE_DRM`.
- `app/version.txt` is stamped with the release suffix at build time.

The gamescope WSI Vulkan layer json is kept from Flathub: in gaming mode it
lets Vulkan present directly to gamescope instead of through XWayland.

## Build

```sh
flatpak install flathub org.flatpak.Builder org.kde.Sdk//6.10 org.kde.Platform//6.10
flatpak run org.flatpak.Builder --user --force-clean --sandbox \
    --install-deps-from=flathub --ccache \
    --repo=repo builddir packaging/flatpak/com.moonlight_stream.Moonlight.json
flatpak build-bundle repo Moonlight-VRR.flatpak com.moonlight_stream.Moonlight master
```

## Install (Steam Deck)

```sh
flatpak install --user ./Moonlight-VRR.flatpak
```

or open the file in Discover. It installs as
`com.moonlight_stream.Moonlight` (branch `master`), side by side with the
Flathub build (branch `stable`) and sharing its configuration and paired
hosts. Add it to Steam with `steamos-add-to-steam` or via Discover to use it
in gaming mode.
