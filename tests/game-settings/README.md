# Per-game streaming settings regression tests

These tests compile the production `StreamingPreferences`, `GameStreamingSettings`
and CLI parser. UI cases load the production `main.qml`, `AppView.qml`,
`GameSettingsView.qml`, and `SettingsView.qml`, with only their artwork resources.
The test target needs no streaming submodules or generated shaders. Host discovery,
profile selection, controller enumeration and platform probes use deterministic doubles. QSettings
uses a temporary INI directory; tests never modify real profiles or contact Sunshine.

## Run

With Qt and its matching compiler available:

```sh
mkdir -p build/game-settings-tests
cd build/game-settings-tests
qmake ../../tests/game-settings/game-settings.pro
make -j4
QT_QPA_PLATFORM=offscreen QT_QUICK_BACKEND=software QT_QUICK_CONTROLS_STYLE=Material ./tst_game_settings
```

Use `mingw32-make` and `release/tst_game_settings.exe` with Windows MinGW.
With an x64 MSVC Qt `bin` directory on PATH, the repository-root command
`scripts\test-settings.bat` builds and runs this suite, the navigation suite,
and the profile suite.
The Windows workflow compiles it in the existing opt-in test tree and runs the
three suites before packaging.

## Coverage

- Exact no-override behavior, detached copies, non-QML packet size, guarded save.
- Active-profile inheritance and separation by profile, host UUID and app ID.
- Sparse values, explicit false values, atomic resolution and bitrate policy.
- Reset one/all, repeated save and intentional matching overrides.
- Corrupt stored types/ranges/enums fall back to profile values; profile-only
  fields cannot be overridden. Unsafe storage identifiers are rejected.
- Stale profile context, invalidated editors and host-scoped cleanup.
- CLI precedence, flags equal to the base, explicit bitrate and YUV444 calculation.
- Sparse round-trip of enum properties and explicit false values.
- Nonary VRR inheritance/overrides, valid latency modes 0/1/2, invalid modes,
  profile-only tracing and exclusion of operational diagnostic state.
- PyroWave codec 5, manual bitrate up to 3,000,000 Kbps, automatic HDR/chroma/FPS
  calculations, final CLI codec selection and explicit bitrate priority.
- Opening/closing QML without accidental overrides and saving on window close.
- Actual checkbox, FPS, bitrate and system-key controls.
- Controller-equivalent Tab/Space/Return/Menu/Escape input, reset confirmation,
  rapid Back, editor destruction, context-menu removal and focus after reordering.
  Resetting one setting preserves other pending edits.
- VRR controls/reset preserve FPS and the saved window mode; FPS choices use
  the draft and game editing does not refresh platform network-buffer services.

These are not hardware-controller, real-host, decoder or live-stream tests. Full
app compilation checks the production AppModel/CLI/Session integration separately;
launch/resume negotiation and platform-specific rendering require hardware checks.
