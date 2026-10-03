# Source-offset recovery at lower frame rates

## Scope and evidence

This follow-up changes the shared controller from task base
`5b6ecb1c01b475b2aa60e0e22e58f30f294b8fd5`. It does not raise cold-start
padding or enlarge any decoded-frame queue. Windows DXGI and Linux Vulkan both
use the corrected mapper; backend-specific GPU synchronization and presentation
modes are not modified.

The supplied `VRR-Logs.7z` contains one trace:
`VRR-Logs/20260915-154338-342-e938fa9f/Moonlight.vrrtrace`.
Its SHA-256 is
`b6d140744e5cd0c278fc02bdd6b77af01bc7eca0118b1ccfa6321398255293ca`.
The decoded header/rows hash matches the footer:
`e0874fd3f93aafae37a758cd5d89f81ae9960321209c0863a032058e7f715e24`.
All 1448 arrival sequence numbers are present; the footer records clean shutdown
with no dropped diagnostic rows. The two ordinary log files are empty.

This is schema 5 with 442 columns, recorded before the current GPU-readiness
lead controls. It is a 60 FPS, 120 Hz DXGI session with the older captured
Balanced parameters. Relative to the earliest pacer admission:

| Window | Presented frames | Target already elapsed | Applied padding |
| --- | ---: | ---: | --- |
| 8 to 16 seconds | 480 | 235 | 8333 to 12697 us |
| 16 to 24 seconds | 480 | 4 | 12697 us |

Frame 481, at approximately 7.963 seconds, has the session's lowest observed
readiness-minus-RTP offset: approximately 150531 us. It is explicitly marked
`phase_discontinuity=1` and `cadence_eligible=0`. Nevertheless the old mapper
admits it to the three-second minimum window. The applied mapping continues
moving earlier through the beginning of the stable 60 FPS failure window,
even as readiness observations have moved later. The original controller also
limits correction to 20 us per frame: 1200 us/s at 60 FPS versus 2400 us/s at
120 FPS. These are measured trace/code observations, not a candidate replay.

The trace cannot establish optical tearing: raster sampling was off, and the
native display evidence does not establish the exact scanout event. The buffer
increase and the offset convergence overlap, so the late-count improvement is
not evidence that every low-FPS stream needs a nearly full-frame buffer.

## Changes and bounds

`playout_offset_cadence_gate=1` prevents cadence-ineligible samples from
training the offset floor. A phase discontinuity clears the observation window
but retains the applied offset. It ends unrestricted epoch warmup so subsequent
observations cannot jump to a different phase immediately. No source-rate flag
alone injects padding; neither the adaptive delay nor the interval buffer is
reseeded. A genuine timeline rebase still initializes a new epoch normally.

`playout_offset_slew_us_per_second=2400` normalizes steady-state correction to
elapsed sender/RTP time. Nominal limits are 80 us/frame at 30 FPS,
40 us/frame at 60 FPS, and 20 us/frame at 120 FPS. The independent
`playout_offset_maximum_step_us=100` bounds any one update, including a long
idle interval. Fractional microseconds are retained to avoid truncation at high
FPS. No whole-step debt is banked after a cap. Ineligible observations discard
fractional credit; clock rollback conservatively clears observations without
moving the mapping, and rebase clears all clock/credit state.

`playout_offset_source_clock=1` derives both aging and slew credit from the
unwrapped RTP timeline. The value remains `decodeCompleteUs - rtpUs`, so actual
readiness movement is still observed, but local decode, renderer, and GPU stalls
cannot buy a larger mapping correction. Captures made with the initial elapsed-
slew implementation retain `playout_offset_source_clock=0` and replay their
worker-decision clock exactly.

The display-period startup clamp is unchanged. So are preset caps, buffer
release/hold rates, the three-waiting-plus-one-active capacity, GPU readiness
lead, per-frame latch selection and software spacing floors. No late frame is
automatically forced into synchronized presentation. On Linux the persistent
Mailbox, Immediate or compatibility FIFO choice remains untouched. Existing
bounded texture-completion polling and failure recovery are retained.

Correcting a persistent late mapping does move future presentation slots later.
This is not free latency removal: the source/readiness phase must be accounted
for somewhere. It avoids pre-allocating a source frame of padding on every
60 FPS connection. Clearing the old minimum on an isolated host phase event
can also temporarily replace a very favorable sample with newer, slower ones;
the slew cap limits that effect, but fresh live latency measurements remain
necessary before claiming a net improvement.

## Historical replay and validation status

Both new enabling fields have schema default zero. Old captures retain the
original sample aging, unconditional observations and 20 us/frame behavior.
The common parameter macro supplies trace columns and JSON serialization;
replay's absent-column fallback uses those historical defaults. No schema
version bump is required. No change is made to historical native presentation
arguments or GPU-readiness adaptation.

Added regression source covers equal elapsed-time correction in both directions,
fractional credit, an observation-clock gap, clock rollback/rebase, a poisoned
phase minimum, unchanged startup targets/padding for all presets at
30/60/116/120 FPS, and both latch capabilities. Parameter round-trip/validation
and fresh trace-field assertions are included. Legacy policy helpers explicitly
disable the new mapper so their historical contracts remain isolated.

The task's sandbox rules prohibit builds, tests and dependency installation.
The trace was decoded and its integrity and statistics were checked; source,
parameter wiring and cumulative diff were reviewed. No compiler, deterministic
suite, replay baseline, candidate sweep or live device test was run. No Windows
or Linux binaries were produced or deployed. Runtime and optical improvement
remain unverified.

## Required follow-up validation

After applying the cumulative result, build the app and VRR diagnostics using
the repository's platform-specific workflow. Run the controller, pacing-worker,
replay-config and rate-policy suites, plus the Linux presentation/swapchain
suites where supported. Rebuild diagnostics after any further source changes.

First verify the exact supplied trace using the freshly built replay utility:

```text
vrrreplay <supplied-trace> --require-exact-baseline --output offset-baseline.json
vrrreplay <supplied-trace> --config tests/vrr/configs/offset-recovery-60-on-120.json --output offset-comparison.json
vrrreplay <supplied-trace> --output current-session-policy.json
```

The versioned comparison pins the capture's complete controller snapshot and
separates unchanged, cadence-gate-only, elapsed-slew-only and combined arms.
It deliberately does not turn on newer GPU-readiness behavior in the old trace.
The separate session-policy invocation exercises all current production defaults.
Check exit codes, exact-baseline status and capture integrity before interpreting
any comparison. Do not force the DXGI capture into a fictitious no-latch Linux
baseline. Compare presented jerk and intended-spacing error alongside full
immutable-decoder-output-to-submission latency, retained delay, drops and
saturation. A lower deadline-miss count alone is not proof of improvement.

Capture clean startup and desktop-to-game transitions at 30/60 FPS and near
refresh on Windows and Linux. Include repeated transitions and a sustained late
readiness baseline. Linux checks must cover ordinary Wayland Mailbox and an
available Immediate/non-latch path without changing swapchain modes midstream;
Gamescope/native display behavior requires a real device. Verify Vulkan readiness
timeout/cancellation recovery remains intact. Inspect both stable latency and
transition tails before promoting the candidate, and keep visual tearing reports
separate from software deadline diagnostics.
