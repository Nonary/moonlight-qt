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

The Linux Deck overlay fixture links the production release formatter and
injects a capture clock. Run it from the qmake release build's `app`
directory after building Moonlight:

```sh
tests_vrr=/absolute/path/to/moonlight-qt/tests/vrr
make -f Makefile.Release -f "$tests_vrr/endtoend-overlay-test.mk" \
  DEFAULT_TIMING_TEST_DIRECTORY="$tests_vrr" endtoend-overlay-test
./endtoend-overlay-test
```

It checks the final row in compact and detailed stats, including the three
endpoints, opt-out, and temporarily unavailable data.

`tst_phasetrajectory` covers the policy-1 decode-buffer feedback regression:
late blocking/nonblocking completion checks, cold and warm GPU estimates,
retained startup protection, prompt real GPU stalls, actual readiness floors,
and completion arithmetic near the timestamp limit. `tst_vrrcacheidentity`
checks that the corrected estimator cannot load a prior saved startup delay.
It also pins that a blocked D3D11 decode fence and the render work beyond the
target's own allowance set the buffer,
while unblocked collection stays queue-neutral and other backends keep their
estimate. `tst_inputgoalintegration` pins that a stall beyond the cap spends no buffer
however many the window holds, that an interval loses at most itself, that one
late frame in a window that has only begun to fill buys nothing while sustained
lateness is held to its rate, that release waits out its hold, that
sustained jitter beyond the cap still reaches it, and that the aim's margin
costs at most 500 us beyond what the goal itself needs.

## Virtual buffer simulator

Run `vrrbuffersim` for the fast development gate. On this Windows checkout the
helper builds only the simulator and passes through its arguments. From the
repository root:

```powershell
.\scripts\test-vrr-buffer.cmd
.\scripts\test-vrr-buffer.cmd --list
.\scripts\test-vrr-buffer.cmd mixed --output build/buffer-before.json
# After a controller edit, rerun the identical virtual stream.
.\scripts\test-vrr-buffer.cmd mixed --compare build/buffer-before.json --output build/buffer-after.json
.\scripts\test-vrr-buffer.cmd sustained-jitter --target 99.90 --oracle 0,500,1000,1500,2000,3000,5000
.\scripts\test-vrr-buffer.cmd --stress --budget-ms 30000
```

On Linux or macOS, build `tests/vrr/buffersim.pro` in a qmake shadow build or
use the binary from the full test build. Run `vrrbuffersim --help` for the
available settings. Running the binary directly skips the build check.
The default suite checks named Balanced workloads and the simulator's accounting,
repeatability, queue feedback and fixed-buffer reference. The stress tier adds
a 30-minute run and a repeatable matrix from 30 to 240 FPS. CPU wall time is
reported for each simulation. Use `--budget-ms 5000` for the fast-suite
performance check on an optimized build.

The sustained-jitter gate also checks a 99.90% target after 75 virtual
seconds of clean input. It bounds time below goal, reserve cost against fixed
trials, reversals and drops. A separate 450 ms jitter burst checks that the
proposal expires when clean delivery resumes.

The 95% checks compare identical clean, queued and jitter-to-clean streams
against Balanced and a fixed zero reserve. They bound latency relative to the
zero-reserve stream even when both goals settle at zero. They also check
qualified time at goal, cadence, drops
and reversals. The report includes per-presented-frame means for decoded queue
residence (admission to dequeue), decode readiness to modeled display, and
source production to modeled display. Queue residence can reach zero while
the pacer still holds a frame it has dequeued.

The production-controller contracts `start-release` and `forecast-goal-recovery`
measure how long protection takes to build and clear. The latter compares the
live policy with revision 13 on identical 120 FPS input with a 5 ms delivery
spike every 11 frames, followed by clean delivery. It measures time to 4 ms of
reserve, early stretches, and decode-to-handoff latency. The noisy-input check
counts material reversals of at least two configured tolerances. The timing
controller suite separately checks both directions of the per-frame budget.

Run `scripts\test-vrr-buffer.cmd ceiling-forecast` to check above-goal buffer
growth at 120 FPS / 120 Hz. This synthetic Low Latency workload starts under
delivery pressure. After pacing recovers to 99%, delivery spikes return for
three seconds, clear, and later persist for 40 seconds. At the first above-goal
request crossing 2.75 ms in the short burst, the simulator reruns the same
history and limits the adaptive maximum just before that frame's submission
feedback. Both branches continue through the ordinary queues, scheduler and
modeled display. If the controller avoids that purchase, the comparison begins
at the burst onset.

The `forecast_comparison` JSON reports time at goal, minimum score, mean
reserve, decode-ready-to-submit and decode-ready-to-display time, and p99
display interval jerk for each branch and span. The gate requires complete
score coverage and zero drops. It fails if the smaller reserve sustains the
goal throughout the brief comparison, saves more than one tolerance of mean
reserve, and keeps display latency and p99 jerk within one tolerance of the
adaptive branch. The sustained phase checks that extra reserve can be necessary
when the disturbance continues.

The `display-ceiling` simulator gate includes 60 seconds of actual 120 FPS
at 120 Hz on the waitable DXGI model, both clean and with 1 ms delivery
spikes every 125 ms. It checks decode-ready-to-display latency, the 99%
pacing goal, display jerk, reserve reversals and skips. The extended
`display-ceiling` contract also compares a cached 4 ms start against
revision 14. Eight 100 FPS source intervals interrupt an otherwise locked
120 FPS stream. The test checks when the reserve clears and how much display
latency the controller recovers from that brief headroom without a skip.

The `reconnect` gate also exercises short connections. A clean 17-second
120/120 session saves its qualified zero reserve, a 2-second session ends
before qualification, and recurring delivery spikes can earn a nonzero
startup reserve. JSON reports expose the first applied reserve and the
largest first-frame reserve across reconnects.

Run the surplus-drain regression for the reported 99% Low Latency configuration with:

```powershell
.\scripts\test-vrr-buffer.cmd surplus-drain --preset low-latency --target 99 --output build/surplus-drain.json
```

The default fast and stress gates run this workload at 95%, 99%, 99.50%
and 99.90%. The host delivers 118 FPS on a 120 Hz display, with 3 ms of
extra transport delay once per second throughout the 120-second session.
The 95% run starts at zero reserve and checks that these misses earn no
additional reserve while the pacing score stays at goal. The 99.90% run
bounds total and continuous time below goal, including before it settles.
Each adaptive run and its fixed-reserve trials receive identical source frames
and delivery costs. The gate scores the final 30 seconds, after startup has
settled. The smallest tested fixed reserve with zero drops, at least 99%
scoring coverage and at least 99% of qualified time at goal is the reference.
The adaptive controller must meet the same quality bounds and settle within
250 µs of the reference for both mean reserve and mean decode-ready-to-display
latency. During continuous scored time strictly above goal and more than
250 µs above the reference, it must make a net 250 µs of downward progress
within 20 seconds. The JSON records the longest such wait in the adaptive run's
metrics and lists each fixed trial under `steady_drain_reference`.
The steady pacing target is evaluated separately from interval-jerk metrics so
the amount of timing protection reclaimed by a lower reserve is measurable.

The default surplus-drain gate also runs the 99% Low Latency profile with
5 ms of extra delivery delay every 500 ms, a 120 FPS request and a 118 FPS
source on a 120 Hz display. It compares settled display wait with fixed
reserves, bounds reserve oscillation, then checks that protection grows
when the same delivery spike occurs every 125 ms. At 95%, 97% and 99.50%
it compares the adaptive run against adjacent fixed reserves that straddle
each goal on that frequent-spike workload. With spikes every 500 ms the same
gate requires 95% to settle at zero and 98.30% at 500 µs. The jitter-to-clean
check compares sustained jitter at 95% against fixed trials from zero to
1,000 µs in 250 µs steps.

In the matched-refresh mixed workload, one replacement is permitted only
when the same simulation presenting every frame shows at least 250 microseconds
of additional mean decode-ready-to-display latency. The adaptive run must
sustain the selected goal for 99% of qualified time and have 99% coverage.

To design an experiment, export a built-in scenario, edit the JSON, and run it:

```powershell
.\scripts\test-vrr-buffer.cmd mixed --export build/my-stream.json
.\scripts\test-vrr-buffer.cmd --load build/my-stream.json --output build/my-results.json
```

A schema-1 workload has a seed, stream rate, display rate, and ordered phases.
Each phase specifies a positive duration in microseconds. Its optional settings
apply only to that phase. For example, this injects one 8 ms network spike every
two seconds for 30 seconds. The final phase measures recovery from the last spike.

```json
{
  "schema": 1, "name": "periodic-network-spike", "seed": 42,
  "stream_hz": 120, "display_hz": 144, "recovery_start_us": 60000000,
  "phases": [
    {"duration_us": 30000000},
    {"duration_us": 30000000, "network_spike_us": 8000, "disturbance_period_us": 2000000},
    {"duration_us": 150000000}
  ]
}
```

The `encoder-backlog` case keeps capture and RTP at 120 FPS on a 144 Hz
display. Frames normally take 2 ms to encode. For 30 seconds one encode per
second takes 40 ms longer, so the following captures queue and then send
2 ms apart until the encoder catches up. The final 150 seconds use clean
encoder timing. Run it with `scripts/test-vrr-buffer.cmd encoder-backlog`.
`encode_us` and `encoder_stall_us` configure these costs, with both defaulting
to zero. Reports with encoder timing include the peak capture-to-send delay
and the count of send intervals shorter than their matching capture intervals.
This workload selects the waitable DXGI model via `dxgi_waitable`. It admits
at most two outstanding presents and stalls worker preparation when that queue
is full. The source and decoder continue in the meantime. The report includes
peak pending presents, worker time waiting for admission, and admission drops.
Display cadence still follows the frames with a minimum refresh interval.

For matched stream and display rates, use `--gap-recovery-reference` to check
which ceiling replacements could have filled an earlier display slot. The
reference uses the next baseline flip's measured time to place an already
decoded replacement one refresh earlier. It requires enough time to prepare
the frame before the next baseline render, an available presenter slot, and
the same or lower local jerk peak and count above 2 ms. All baseline frames
keep their original display times. This reference uses future flip evidence
and is separate from the online controller metrics.

```powershell
.\scripts\test-vrr-buffer.cmd encoder-backlog --display 120 --gap-recovery-reference
.\scripts\test-vrr-buffer.cmd encoder-backlog --fps 144 --display 144 --gap-recovery-reference
```

The reference reports display gaps and jerk for frames captured in the
encoder-stall phases, or the full session if there are no such phases. Drop
counts and preserved frame counts cover the whole session. It also reports
the oldest inserted frame's capture-to-display age. The stress gate checks
the reference with 250, 750 and 1,500 microseconds of delivery jitter at both
refresh rates.

`vrrstreamsimulator.cpp` advances a virtual clock to source, delivery, decode,
worker and presentation events. It runs the production controller and drop
policies. A changed target occupies the worker for a different duration, which
changes the queue, replacements, future submissions and display feedback.
Built-in phases also exercise independent source cadence changes, a serial
decode bottleneck, RTP jumps, reconnects and the display ceiling.
The display is a modeled immediate or latched presenter. The waitable DXGI
model covers admission and minimum spacing. Native raster state and detailed
compositor timing require a capture. Generated pictures are
independently decodable after a queue overflow. Predictive-codec recovery,
native scheduled presentation, packet-level transport and physical scanout
require separate integration evidence.

**Reading a run.** At-goal percentage divides qualified time at goal by all
qualified time. Read it alongside coverage, the fraction of post-calibration
time that qualified. The JSON partitions the whole virtual session into
calibration, unavailable, at-goal and below-goal microseconds. It also records
the longest continuous shortfall and time to the first sustained second back at
goal after the declared disturbance. Reserve, queue residence and source-to-display
latency are separate measurements. Presented jerk uses changes in consecutive
modeled display intervals. Hitch magnitude compares display spacing with the
same two source frames. Drop counters account for frames that never reached display.

**Reading an oracle or comparison.** A fixed-buffer trial uses the same
producer seed, presentation model and production scoring as the adaptive run.
The oracle reports the smallest *tested* reserve that meets the timing target
for 95% of qualified time, with at least 90% post-calibration coverage and zero
drops. The JSON includes individual candidates. An empty result means the grid
had no qualifying size. For comparisons, phase definitions, seed and policy
options must match. Numeric deltas are candidate minus baseline. Catalog
workloads have policy-specific assertions. Custom inputs and overrides check
the model invariants and report the resulting policy tradeoffs.

## Extended regression contracts

`tst_vrrbufferregressions` preserves the historical buffer contracts and
recorded-span comparisons. It exercises the live interval controller on latching and
non-latching presenters, with Reduce Judder on and off. `interval-pressure`
follows earned protection through a full score history and clean release.
`rate-round-trip` checks buffer continuity and presented cadence through a
90–30–90 FPS transition. Ceiling comparisons score the same arrivals through
the VRR and fixed-refresh models. Its
`slow-gpu-decode` scenario replays a recorded Radeon 780M span
(`slowdecodespan.h`, decode close to the 138 FPS source interval) through a
serial decoder and worker, and requires a settled buffer and source mapping
when the presenter timestamps decode completion; it prints the result with
collection times only beside it. `tst_dxgipresent` covers the watcher that
takes those timestamps, `tst_vrrtimingcontroller` that a timestamped
completion maps and sizes the same however late the worker collects the
frame, and `tst_vrrpacingworker` that it reaches the controller and the
`decode_completion_exact` trace column.

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

`variable-source-judder` runs the 99% Low Latency profile on a 120 Hz display.
Its 18-second producer repeats 80 consecutive host RTP intervals from the
reported variable-rate session. Delivery, decoding and presentation use the
simulator's own clocks. The optional phase field `source_intervals_us` holds
a repeating sequence of positive gaps and replaces the fixed host period.

`extra_delay_cycles` counts complete trips from at most 0.5 ms of planned
extra delay to at least 2 ms and back. `sustained_extra_delay_cycles` counts
the same trips on a four-frame moving average to distinguish retained delay
from individual short/long frame corrections. Both measurements restart
across dropped frames and reconnects. The regression also checks presented
interval changes above 2 ms, score coverage, input buffer, time-weighted
extra delay and decode-ready-to-display latency.

`uneven-game-cadence` uses 80 consecutive RTP gaps from a roughly 70 FPS game
on a 120 Hz display with independent transport jitter. The Balanced gate checks
the frequency and 99th percentile of presented interval changes, sustained
extra-delay cycles, input buffer, decode-ready-to-display latency and pacing.

`rate-drop-hitches` steps from 120 to 60 FPS on a 120 Hz display with Reduce
Judder off. It replays 40 consecutive host intervals from the slow segment
with independent delivery jitter, then returns to steady 60 FPS. The gate
checks the input buffer's 95th percentile and time at the pacing goal.
Separate runs give the settled stream a 2 ms maximum. Recurring 6 ms and
10 ms deliveries must count the same number of late frames and purchase zero
reserve. Recurring 1.8 ms deliveries must earn protection and miss fewer frames.

## Current-policy scenarios

Replay configurations use `config_schema: 2` and `base: "session"` to derive
the current production policy from the capture's settings and presenter. The
recorded baseline retains its captured current parameters. Revision-specific
historical policies and schema-1 configs are unsupported. `smoothing_mode`
selects off (0), recorded worker phase-locked (2), native tolerance-bounded (4),
Absorb (7), recorded worker interval smoothing (8), recorded worker source-phase
smoothing (9), or worker source-phase smoothing with bounded retiming at the
display ceiling (10).
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
