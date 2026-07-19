# VRR deterministic tests

The VRR test tree is opt-in so regular application and package builds do not
gain test targets. From an out-of-tree build directory, configure it with:

```powershell
& D:\Qt\6.10.1\msvc2022_64\bin\qmake.exe ..\tests\tests.pro CONFIG+=tests
nmake
.\vrr\release\tst_vrrtimingcontroller.exe
.\vrr\release\tst_vrrratepolicy.exe
.\vrr\release\tst_vrrpacingworker.exe
```

On Linux, use the selected Qt `qmake` in the same way and run `make`. The
timing-controller executable covers the platform-neutral source pacing policy
and only links `libavutil`; it deliberately does not
create an SDL window, decoder, renderer, network connection, or Qt event loop.
The policy executable is an app-less QtTest binary and only compiles the pure
FPS policy source.

The timing-controller executable also exercises cumulative RTP cadence
learning for every integer rate from 30 through 116 FPS on a 120 Hz-quantized
capture clock, a continuous one-FPS-per-second sweep in both directions,
isolated hitch rejection, rapid 30 FPS cutscene transitions, high-rate
recovery, and smooth projected targets from mixed 8.33/16.67 ms timestamp
intervals.

`tst_vrrpacingworker` supplies a fake frame presenter and a test-owned
`LiGetMicroseconds()` epoch. It verifies the bounded worker queue,
drop accounting, minimize/restore discard and fresh-frame behavior, deferred
AVFrame lifetime, presenter eligibility rejection, display-period spacing,
native submission timing across pre-submit work and blocking
returns, and the minimal prepare/present/cancel contract. D3D11 completes
queued rendering behind a GPU fence before the worker waits for its target,
then submits immediate frames with `DXGI_PRESENT_ALLOW_TEARING`. This keeps the
timed CPU submission boundary adjacent to a displayable back buffer while the
worker's display-period floor remains the tear-avoidance authority. Linux
presentation mode remains an immutable renderer choice selected when its
swapchain is created. The native backends still need their platform-specific
integration runs.
When collecting `MOONLIGHT_VRR_TRACE` on Windows, use a local file and copy it
after the session; direct UNC trace paths are rejected to keep network I/O off
the time-critical pacing worker.

Set `MOONLIGHT_VRR_DEEP_TRACE=1` only for native flip diagnosis. It adds
native-call timing, DXGI present/frame-statistics values, and `gpu_ready_*`
timing that proves queued rendering completed before the target boundary. The
extra queries and fields are observation-only and do not alter pacing decisions.
Trace formatting remains on the background writer thread, and every trace is
capped at 128 MiB so an accidentally long session cannot write indefinitely.

To include these targets in a top-level developer build, the integration
project should add `tests` to its `SUBDIRS` only inside
`contains(CONFIG, tests)`. That wiring is intentionally outside this test-only
directory.
