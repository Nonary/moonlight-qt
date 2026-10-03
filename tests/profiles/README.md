# Profile regressions

Six focused Qt Test cases compile the production `ProfileManager`,
`StreamingPreferences`, `NvHTTP` and `NvAddress`. They verify migration of legacy renderer/controller/VRR
settings and profile ID precedence over display names, including case-insensitive
CLI lookup, and profile-change signal ordering around state replacement, removal
and shutdown.
Before a profile is selected, loading preferences uses defaults and saving them
leaves the existing settings untouched.
They also verify the HTTP client's UID/SSL snapshot and request cancellation
around profile changes, including requests with no timeout, cancellation before
the event loop starts and replies that complete during cancellation. A fake
network manager/reply sends no network traffic.
Credential generation, cache location and platform probes are doubles.
The target's VRR rate policy and diagnostic helpers are linked as production code;
these tests do not start a stream or export diagnostics.
All settings use a temporary INI directory.

With Qt and its matching compiler available:

```sh
mkdir -p build/profile-tests
cd build/profile-tests
qmake ../../tests/profiles/profiles.pro
make -j4
./tst_profiles
```

On Windows, use `nmake` or `mingw32-make` and `release/tst_profiles.exe`.
`scripts\test-settings.bat` builds and runs this suite and the profile navigation suite.
The existing test tree also includes both targets when invoked with `CONFIG+=tests`.
The `moonlight-common-c` headers must be present; its library is not linked.
An isolated header checkout can be supplied to qmake with
`MOONLIGHT_COMMON_C_INCLUDE=/path/to/moonlight-common-c/src`.
