# Navigation regression tests

This standalone Qt Test target loads the production QML screens and GUI artwork.
Only the backend singletons and host/game models are replaced by in-memory QML
fixtures, including idle PyroWave calibration and network-buffer services.
It does not read user profiles, connect to hosts, or start streams.

Build with a Qt kit containing Quick, Quick Controls 2, and Test:

```sh
mkdir -p build/navigation-tests
cd build/navigation-tests
qmake ../../tests/navigation/navigation.pro
make
QT_QPA_PLATFORM=offscreen QT_QUICK_CONTROLS_STYLE=Material QT_QUICK_BACKEND=software ./tst_navigation
```

On Windows use the selected kit's `qmake`, `mingw32-make` or `nmake`, and put its
compiler and Qt `bin` directories on `PATH`. Set the three variables above using
PowerShell's `$env:` syntax and run `release/tst_navigation.exe` (or `debug/`).
With the offscreen platform, `QT_QPA_FONTDIR=C:/Windows/Fonts` supplies system fonts.

Coverage:

- Auto-login gives focus to the host grid immediately.
- Repeated Escape (controller B) and platform Back during a transition perform
  only one back operation. After the transition, navigation can continue to
  profiles and select another profile.
- Hidden profile pages cannot reclaim focus or activate another profile.
- A late host-loss callback cannot pop an unrelated page.
- Removed game pages and their models are destroyed.
- Profile/host/settings navigation cannot interrupt an ongoing transition.
- Dismissing the quit dialog restores controller navigation on profiles.
The tests exercise the Qt keys emitted by `SdlGamepadKeyNavigation`, not a physical
controller or SDL device polling. A device-level check with Sunshine is still a
useful release check. They run separately from the streaming application's build.
