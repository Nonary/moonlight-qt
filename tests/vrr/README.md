# VRR deterministic tests and replay

The opt-in test project exercises the timing controller, source cadence, input
buffer, pacing worker, replay, and renderer policies. It is separate from the
application build. Run the suites relevant to a change, and follow `AGENTS.md`
for the complete Windows VRR build and deployment checks. On Linux, configure
the target inside the development container, for example:

```sh
mkdir -p build/vrr-tests
cd build/vrr-tests
qmake6 ../../tests/vrr/vrr.pro CONFIG+=tests
make -j2
```

`productionpolicybaseline.pro` records exact deterministic fingerprints across
production presets, smoothing modes, and presentation capabilities. These are
regression checks of the controller model, not measurements of screen output.
The input, source-gap, phase-trajectory, timing-controller, worker, profile,
replay-config, and performance-statistics tests cover their respective current
contracts. The worker fixture can export a fresh trace for replay verification:

```sh
MOONLIGHT_VRR_TEST_EXPORT_TRACE=fresh.vrrtrace ./tst_vrrpacingworker
vrrreplay fresh.vrrtrace --require-exact-baseline --output baseline.json
python3 ../../tests/vrr/check_fresh_trace_schema.py ./vrrreplay fresh.vrrtrace
```

Use the actual paths to the worker and replay binaries in your build tree.
The fixture is synthetic and does not establish live smoothness or scanout.

`tst_phasetrajectory` covers the policy-1 decode-buffer feedback regression:
late blocking/nonblocking completion checks, cold and warm GPU estimates,
retained startup protection, prompt real GPU stalls, actual readiness floors,
and completion arithmetic near the timestamp limit. `tst_vrrcacheidentity`
checks that the corrected estimator cannot load a prior saved startup delay.

## Capture and exact replay

The worker writes schema-6 captures as compressed `.vrrtrace` files or CSV.
Replay requires schema 6 and all current controller parameters; older captures
must be recaptured for controller replay. `scripts/decode-vrr-trace.py` still
decodes older compressed traces for inspection. Trace capture is asynchronous
and bounded, so inspect the clean-close footer, dropped rows and sequence
integrity before treating it as complete.

```sh
vrrreplay capture.vrrtrace --require-exact-baseline --output baseline.json
```

Check the process exit code and both `capture.recorded_sequence_integrity_valid`
and `fidelity.baseline_exact` in the result. Exact replay reproduces the
recorded controller and execution model; it is not a measurement of physical
scanout or proof that a changed policy would cause the same host, network, GPU,
or display events. Use a fresh matching capture and preserve its baseline
separately from candidate results. Re-enumerate captures before each analysis
as described in `AGENTS.md`.

The 2026-10-30 correction changes the existing policy-1 accounting in place.
Captures recorded before that correction can be rejected for divergent
diagnostics or original deadlines despite retaining the same policy fields.
Preserve an exact baseline from the matching old executable and use fresh
worker captures for the corrected controller's replay and stress gates.
Never waive an exact mismatch or introduce a second production policy merely
to reproduce the old bug.

## Current-policy scenarios

Replay configurations use `config_schema: 2` and `base: "session"` to derive
the current production policy from the capture's settings and presenter. The
recorded baseline retains its captured current parameters. Revision-specific
historical policies and schema-1 configs are unsupported. `smoothing_mode`
selects the current off (0), worker phase-locked (2), native tolerance-bounded (4)
or Absorb (7) mode; other values and retired revision selectors are rejected.
The production and interval policy baseline targets preserve 54 deterministic
pre-cleanup output fingerprints each. List available fields
with `vrrreplay --list-parameters`; `tests/vrr/replay-scenarios.example.json`
shows the configuration and assertion syntax.

```sh
vrrreplay capture.vrrtrace --config tests/vrr/configs/playout-delay-sweep-lowlatency.json --jobs 0 --output sweep.json
vrrreplay capture.vrrtrace --config tests/vrr/configs/production-stress.json --jobs 0 --output stress.json
```

The second sweep introduces decision, preparation/submission, and scheduler
faults and checks modeled interval safety and explicit latency ceilings. A
synthetic fault's latency is not normal operating latency. The low-latency and
smoothness sweeps vary the input buffer cap within current policy. Batch replay
runs independent scenarios concurrently, with an automatic cap of 16 jobs; use
`--jobs 1` when debugging. An assertion failure is an unsuccessful run.

For comparisons inspect presented-interval jerk and sender-spacing fidelity,
as well as latency, drift, interval violations, and any available raster bounds.
A saturated fixed-admission worker model cannot shed frames as the live worker
would; its latency is not a valid policy result. CPU submission spacing, OS
display reports, and physical panel output are separate evidence layers.
Compare gaps to the workload and actual source cadence rather than the requested
FPS alone; idle output and rate changes can legitimately produce long intervals.
Raster models describe stated timing assumptions and are not optical tear
measurements. See `AGENTS.md` for the complete replay workflow and limitations.

## Native macOS Metal and PyroWave integration smoke test

Build this target explicitly after the application has produced
`build/mac-client/pyrowave/libpyrowave.a`. `CONFIG+=pyrowave` opts into the GPU
codec cases; the ordinary native target does not compile the codec. Run from
the repository root:

```sh
nativeRoot="$PWD"
mkdir -p build/mac-metal-pyrowave-smoke
cd build/mac-metal-pyrowave-smoke
"$nativeRoot/build/vrr-hybrid/qt/6.11.1/macos/bin/qmake" \
  "$nativeRoot/tests/vrr/metalpresenter.pro" \
  QMAKE_APPLE_DEVICE_ARCHS=arm64 CONFIG+=release CONFIG-=debug \
  CONFIG+=pyrowave \
  "PYROWAVE_STATIC_LIBRARY=$nativeRoot/build/mac-client/pyrowave/libpyrowave.a"
make -j6
DYLD_LIBRARY_PATH="$nativeRoot/libs/mac/lib" \
GRANITE_VULKAN_LIBRARY="$nativeRoot/libs/mac/lib/libMoltenVK.dylib" \
  ./tst_metalpresenter "$PWD/native-metal-pyrowave.vrrtrace"
DYLD_LIBRARY_PATH="$nativeRoot/libs/mac/lib" \
  "$nativeRoot/build/mac-tests/vrr/vrrreplay" \
  "$PWD/native-metal-pyrowave.vrrtrace" --require-exact-baseline \
  --output "$PWD/native-metal-pyrowave-baseline.json"
```

The test opens a temporary native fullscreen window. On a variable display it
first verifies rejection of windowed and V-sync-disabled VRR, then exercises
the actual Metal presenter with software and VideoToolbox frames. The two
combined codec cases encode 128×96 patterns on the GPU and decode to exported
Metal textures in 4:2:0/8-bit and 4:4:4/10-bit. Their AVFrames have no CPU plane
data. The presenter waits for asynchronous decode, renders, cancels, prepares
again, frees the source frame and decoder, and presents the retained drawable.
A shared-frame fixed/CAMetalDisplayLink case also runs after VRR fallback.

The separate 100-frame worker fixture uses synthetic software frames at 116 FPS
on the 120 Hz panel and records real command-completion and presented-time
feedback for exact replay. On a fixed display the adaptive cases are skipped
after verifying fixed fallback. These tests establish native pipeline behavior;
the PyroWave GPU round-trip suite covers full-resolution plane equality, while
live streaming, external-display scanout and sustained performance require
separate validation.
