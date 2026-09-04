# Flatpak packaging (SteamOS / Steam Deck)

This directory builds the VRR-pacing fork of Moonlight as a Flatpak. The
manifest is derived from the official Flathub packaging and pins the vrr13
application and dependency revisions used for the release build.

The Flatpak builds libplacebo separately from Moonlight. Its module therefore
explicitly applies Moonlight's Gamescope WSI workaround for gamescope#2261;
merely carrying that patch in the Moonlight source commit does not apply it to
the separately fetched libplacebo source.

The gamescope WSI Vulkan layer JSON lets Vulkan present directly to Gamescope
instead of through XWayland in SteamOS Gaming Mode. The manifest also keeps
libdrm enabled so Moonlight can read the real display refresh rate rather than
Gamescope's synthetic XWayland mode.

## Build

Run from the repository root:

```sh
flatpak install flathub org.flatpak.Builder org.kde.Sdk//6.10 org.kde.Platform//6.10
flatpak run org.flatpak.Builder --user --force-clean --sandbox \
    --install-deps-from=flathub --ccache \
    --repo=repo builddir packaging/flatpak/com.moonlight_stream.Moonlight.json
flatpak build-bundle repo Moonlight-VRR-6.1.0-vrr13-linux-x86_64.flatpak \
    com.moonlight_stream.Moonlight master
```

Before publishing, inspect the exported `files/manifest.json` and confirm the
libplacebo module lists
`libplacebo-disable-internally-synchronized-queues.patch` as a patch source.

## Install

```sh
flatpak install --user --reinstall \
    ./Moonlight-VRR-6.1.0-vrr13-linux-x86_64.flatpak
```

The bundle installs as `com.moonlight_stream.Moonlight` on branch `master`.
The regular Flathub build uses branch `stable`; recreate a stale Steam shortcut
after installation so Gaming Mode launches the intended user build.
