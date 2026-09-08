# Standalone VRR tools with CMake

This build runs the existing C++ controller, replay tools and deterministic
regressions on macOS, Linux or Windows. It compiles the production sources
directly; no Windows GPU or Moonlight application build is required. The replay
display model remains an estimate, not an optical measurement. See [README.md](README.md)
for capture fidelity, calibration requirements and replay options.

Requirements:

- CMake 3.21 or newer, a C++17 compiler, and Make, Ninja or Visual Studio.
- Qt 6 development packages for Core, Test and Qml, including `moc`.
- SDL 2 and FFmpeg `libavutil` development headers and libraries.
- SDL2_ttf for the overlay regression (default on).
- The checked-out `moonlight-common-c/moonlight-common-c` submodule, or
  `VRR_COMMON_INCLUDE_DIR` pointing to its real `Limelight.h`.
- Python 3.9 or newer for the optional playback-review runner regressions.

All libraries must match the compiler and target architecture. CMake discovers
SDKs through `CMAKE_PREFIX_PATH`, normal platform paths and, when available,
pkg-config. The existing repository Windows/macOS prebuilt directories are also
searched. This build never downloads or substitutes dependencies.

From the repository root, with dependencies installed or unpacked:

```sh
cmake -S tests/vrr -B build/vrr-cmake -DCMAKE_BUILD_TYPE=Release -DCMAKE_PREFIX_PATH=/path/to/Qt/6.x/platform
cmake --build build/vrr-cmake --config Release --parallel
ctest --test-dir build/vrr-cmake -C Release --output-on-failure
build/vrr-cmake/vrrreplay --help
```

For Visual Studio, configure from its developer terminal, select the matching
architecture (`-A x64` or `-A ARM64`), and use the Qt MSVC SDK. Multi-configuration
generators put executables in `build/vrr-cmake/Release/`. Before running tests,
put the matching Qt `bin` directory and the directories containing SDL2,
SDL2_ttf and avutil runtime DLLs on `PATH`. A link library alone is insufficient
to run a Windows executable.

For dependencies outside normal search paths, pass these cache variables as
`-DNAME=/absolute/path` arguments at configure time:

| Variable | Expected path |
| --- | --- |
| `VRR_AVUTIL_INCLUDE_DIR` | Directory containing `libavutil/frame.h` |
| `VRR_AVUTIL_LIBRARY` | avutil link library (`.so`, `.dylib` or `.lib`) |
| `VRR_SDL2_INCLUDE_DIR` | Directory containing `SDL.h` |
| `VRR_SDL2_LIBRARY` | SDL2 link library or macOS `.framework` directory |
| `VRR_SDL2_TTF_INCLUDE_DIR` | Directory containing `SDL_ttf.h` |
| `VRR_SDL2_TTF_LIBRARY` | SDL2_ttf link library or `.framework` directory |
| `VRR_COMMON_INCLUDE_DIR` | Directory containing the real `Limelight.h` |

FFmpeg headers must include its generated configuration headers. On a
case-insensitive filesystem, avoid adding the entire FFmpeg source root to the
include path: its `VERSION` file can shadow the C++ `<version>` header. A parent
directory containing just a symlink to the configured `libavutil` directory is
sufficient. Application-bundled macOS libraries can have loader-relative install
names; prefer a development SDK with working runtime paths, or repair only the
new test binaries' dependency paths with `install_name_tool`.

The default build includes all nine shared deterministic suites, `vrrreplay`,
`vrrqueuesim`, and their help checks. Set `VRR_BUILD_OVERLAY_TEST=OFF` only if
SDL2_ttf is unavailable; CMake otherwise fails rather than silently omitting
that test. On Linux, Vulkan and Wayland regressions are added when their
development packages are found; configure output identifies missing optional
packages. `VRR_BUILD_PLATFORM_TESTS=OFF` disables these Linux-specific tests.
`ctest -N --test-dir build/vrr-cmake -C Release` lists the actual test coverage.

Build and run tests again after changing production pacing or replay sources.
Use a fresh output directory for another compiler or architecture. CMake's
standalone build does not package, publish or update the installed application.
