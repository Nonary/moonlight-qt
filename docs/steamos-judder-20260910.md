# Gaming Mode judder investigation, 2026-09-10

The FIFO/Mailbox A/B switch took effect, but the user reports that motion still
requires Steam's performance overlay to look smooth. No visual remedy has been
validated. The latest trace shows normal steady decoded-queue occupancy, startup
blocking, uneven source intervals, and insufficient usable compositor feedback
to measure display cadence.

## Evidence selection

Re-enumerated `~/moonlight-logs`, `~/moonlight-vrr-traces`, and `~/vrr-traces`.
The third directory is absent; no ChaseShare is mounted on this Linux client.
Selected the newest completed capture:

```text
/home/deck/moonlight-logs/moonlight-pacing-20260910-200318-1886785.vrrtrace
Length: 720306 bytes
Last write UTC: 2026-09-11T01:06:54.436027+00:00
SHA-256: abd5db7bc51af131bdcb00d270b50cd39824af62cdc014ea40ec00adc90e0c4f
Matched app log: /home/deck/moonlight-logs/moonlight-20260910-200318-1886785.log
Captured executable SHA-256: f3ab12366f87ea19036f8e048b2fc6ab3f51a0fb109befb4f93d72fffc409be7
Source: 4c0ff4b3 plus pre-existing local smoothing and telemetry changes
```

The app log contains three connections. Before the reconnect fix, every new
worker opened the same trace with `wb`, so the detailed trace contains only the
last connection. There is no matching launcher replay sidecar. Earlier connection
summaries below come from this app log, not from a retained frame-level A/B pair.

| Connection | Application mode | Settings | Client processing mean | Queue/pacing mean | Submission score |
| --- | --- | --- | --- | --- | --- |
| First | FIFO, experiment off | Balanced, smoothing on | 14.52 ms | 12.58 ms | 99.9% |
| Second | Mailbox, experiment on | Balanced, smoothing on | 14.06 ms | 12.38 ms | 99.3% |
| Last | Mailbox, experiment on | Lowest latency, smoothing off | 11.35 ms | 9.59 ms | 99.8% |

The last session is Gamescope, X11 through the WSI layer, VAAPI decoding and
Vulkan/libplacebo rendering, HDR10 swapchain output, 120 Hz display/requested
120 FPS. Actual incoming rate is 75.19 FPS. The installed Gamescope package is
3.16.23.5-1. The machine returned to Desktop Mode before this investigation.

## Latest connection findings

- 3,160 frame records over about 42 seconds, 3,119 presentations. All 40
  non-shutdown drops occurred in the first second: 37 capacity evictions and
  3 stale replacements. One final frame was cancelled on shutdown.
- A startup preparation took 363 ms, including 360 ms in the renderer. This
  accounts for a real startup backlog, but does not establish the cause of
  continued overlay-dependent judder.
- For presented frames, admission queue depth p99 was zero. Waiting from pacer
  admission to dequeue averaged 0.079 ms, p99 1.057 ms. Five-second windows after
  startup show about 10.5–12.1 ms mean admission-to-presentation-return time.
  There is no sustained ballooning decoded-frame backlog in this connection.
- Decoder GPU readiness waiting averaged 4.24 ms. Applied playout padding
  averaged 6.67 ms and never exceeded 7.26 ms. Requested reserve reached
  14–17 ms, but the Lowest latency cap prevented it being applied. These timing
  quantities overlap; they must not be added as an end-to-end latency sum.
- The reported 9.59 ms queue/pacing delay is the complement of measured
  rendering within total client processing. It includes GPU-readiness and
  intentional target waits; it is not just waiting decoded frames.
- Raw consecutive RTP intervals across all arrivals averaged 13.346 ms, ranged
  from 5.167 to 18.011 ms, and had **no intervals over 25 ms**. Replay's two
  excluded long source-span pairs cross startup drops; they are not evidence
  of host capture stalls. No network loss was reported.
- Replay's submission-as-presentation jerk exceeded 2 ms for 18.7% of eligible
  pairs, with p95 2.904 ms and p99 4.100 ms. Sender residual p95 was only 0.047 ms
  and p99 0.243 ms. Following uneven host timestamps can therefore score very
  well on client spacing while still producing uneven intervals. These are
  submission proxies, not physical display measurements.
- Only 20 presentation feedback records were usable (0.64% coverage), yielding
  two qualifying native cadence intervals. Missing feedback cannot be counted
  as either displayed successes or dropped frames.

## Replay limitation

Ran the existing current replay in `moonlight-dev` with
`--require-exact-baseline --output build/vrr-baseline-20260910-200318.json`.
Exit code was **3**. Sequence integrity and clean-close accounting passed with
zero missing rows. All 3,123 controller targets and 3,119 simulated submissions
matched, as did controller diagnostics. However, native-outcome validation
rejected all Vulkan presents under the replay's DXGI-oriented strict diagnostic
requirements; `fidelity.baseline_exact` is false. The statistics are exploratory
evidence, not strict A/B proof. No controller parameter sweep was selected.

## Changes and remaining test

The client estimate now reads `Client pacing (estimated)`, with pacing hitches
named explicitly. The Gamescope checkbox names the Mailbox experiment instead
of implying a tested SteamOS remedy. Reconnects archive the preceding trace
before opening the canonical path again, preserving each connection's complete
data. Gamescope timing diagnostics now log returned/emitted records and rejection
reasons at renderer teardown without changing timing acceptance or pacing.

The [scoped composition launcher](../scripts/moonlight-gamescope-composition-test.py)
prepares the next comparison: same stream and scene, overlay off, composition
forced and verified, original value restored on exit. This can isolate a direct
scanout/composition difference; it cannot reproduce every overlay scheduling
effect. See [the launch instructions](steamos-vrr.md#scoped-composition-test-launcher).

Gamescope 3.16.23.5 supplies its scheduled vblank target to
`wlserver_past_present_timing()` as `actualPresentTime`, so the name alone cannot
establish a physical display event. Future/before-submission timestamps remain
rejected. The new rejection counts will show whether this contributes to the
observed sparse feedback. Source: [handle_presented_for_window](https://github.com/ValveSoftware/gamescope/blob/3.16.23.5/src/steamcompmgr.cpp#L6889-L6940).

The actual motion symptom remains unverified until another Gaming Mode comparison.

## Validation and local delivery

Incrementally rebuilt diagnostics and `build/app/moonlight` in the existing
`moonlight-dev` container. This is the executable used by the local launcher;
no Moonlight process was running during the build. New executable SHA-256:
`cf23f32e8f55260cb14cf528a7d52d6d972022570e155d9736409b4babec1433`.

The four required VRR suites, `tst_vulkantiming`, `vrrreplay --help`, and the
application's offscreen `--help` all exited zero. The worker regression verified
three successive connections preserve both earlier complete traces. Eight
launcher tests cover restoration, an already-forced compositor, setup/launch
failures, unknown commands with zero exit status, Desktop Mode refusal, and loss
of the forced-composition condition during a run. `git diff --check` passed.

After the final diagnostic build, reran exact replay with the untouched baseline
as `--compare`, writing `build/vrr-verification-20260910-200318.json`. The verdict
is unchanged with zero metric deltas, and the same exact-gate failure remains.
No timing parameters were changed in this investigation. These checks establish
build and diagnostic behavior; they do not validate live Gaming Mode smoothness.

## Follow-up: buffer ceiling and motion reporting

Re-enumerated the capture directories for the follow-up; the selected file,
size, timestamp and SHA-256 above remain the newest completed capture. A fresh
`build/balloon-baseline.json` exact run exited 3 with the same native diagnostic
limitation; this remains exploratory evidence.

The nominal 16 ms buffer maximum was expanded by twice the fitted source period
before the preset cap was applied. New sessions now disable that expansion,
retaining the smaller preset cap and an absolute 16 ms ceiling. This prevents
slow desktop delivery from expanding the ceiling; it does not prove that desktop
capture caused the reported session's delay. The latest session already remained
below that bound (7.251 ms maximum applied buffer), so this change is not expected
to repair its compositor-dependent motion.

The shared Windows/Linux overlay now also measures changes between consecutive
submission intervals. Using the new calculation on this capture yields 588
changes over 2 ms among 3,117 pairs: 81.1% motion cadence. It includes host
variation and local drop effects, unlike the source-fidelity pacing score.
Physical scanout remains unobserved. Separate averaged queued residence, GPU wait,
and selected buffer allowance identify what the combined queue/pacing line hides;
these fields must not be added together.

Validation: the slow-source regression reproduces 26 ms protection with the old
expanding setting at 20, 30 and 60 FPS; the new setting peaks at 16 ms for each.
The four VRR suites and replay help passed after the final diagnostic build.
The five-scenario exploratory replay (nominal, decision, preparation, submission,
scheduler burst) passed its zero modeled interval violation and <=30 ms p99
latency assertions; p99 values ranged from 7.772 to 10.022 ms. The candidate uses the complete
recorded parameter snapshot with only the expanding maximum disabled; the
untouched-capture comparison has zero metric deltas, as expected below 16 ms. Results live in
`build/balloon-candidate.json` and `build/balloon-stress.json`, with the versioned
input in `build/balloon-stress-config.json`. These simulations do not model the
compositor overlay effect and do not overcome the capture's exact-gate failure.

The local `moonlight-dev` application was rebuilt and its offscreen help smoke
check passed. SHA-256 of `build/app/moonlight`:
`437078c698050c3f7c054527cb30c2ab6af92ac9b6cd4336a5e628dad131817c`.
The composition launcher's eight deterministic tests passed. The user confirmed
that the failed A/B test used only the Mailbox checkbox; forced composition has
not yet been tested live. No compositor or streaming service was restarted.

## Replacement composition checkbox

At the user's request, the failed Mailbox checkbox has been replaced with
**Test forced composition in Gaming Mode**. The new preference defaults off;
the previous Mailbox preference is ignored and removed on settings save. No
Steam shortcut edit is required. The connection guard verifies setup, restores
its own change on normal cleanup and setup failures, and preserves composition
that was already forced. Startup fails visibly if the requested test cannot
be verified. The optional external launcher remains available for monitoring.

The guard's injected-command tests and the four VRR suites plus replay help
passed. The local application rebuilt successfully and passed offscreen help.
Live Gaming Mode composition and visual smoothness remain unverified.
Current executable SHA-256: `d8242ad9b33d6aefae865126ab90920d0a167e41741ebef2a027af66c653901b`.

## Existing smoothing option: same-capture comparison

The next follow-up re-enumerated capture roots and selected the same file/hash.
Fresh exact replay `build/motion-baseline.json` again exited 3. A complete captured
controller snapshot (with the new absolute cap) was used in a single four-scenario
batch, changing only smoothing and the explicitly named preset. No source or
user settings were changed. Results: `build/motion-smoothing.json`.

| Option | Pairs within 2 ms jerk (approx.) | Jerk p99 | Modeled decode-to-submission p99 |
| --- | --- | --- | --- |
| Smoothing off, Lowest | 81.3% | 4.100 ms | 7.772 ms |
| Existing smoothing on, Lowest | 99.4% | 1.828 ms | 8.071 ms |
| Existing smoothing on, Balanced | 99.6% | 1.763 ms | 14.924 ms |
| Stronger experimental smoothing, Lowest | 99.5% | 1.222 ms | 8.855 ms |

Replay scores use 3,114 eligible interval pairs, slightly different from the
new overlay's 3,117-pair startup/epoch accounting (81.1%). Existing smoothing
raises sender-spacing residual p99 from 0.243 to 1.546 ms: it intentionally
retimes uneven source stamps to improve output cadence. Mean modeled latency
changes from 6.369 to 6.376 ms. These times do not measure full client or
end-to-end latency. No candidate was saturated. Stronger smoothing and Balanced
provide little percentage improvement for their higher latency tails, so the
existing Smooth frame timing option plus Lowest latency is the first live test.

The five-scenario stress run for that selection is in
`build/motion-smoothing-stress.json`; all interval-safety and <=30 ms p99
assertions passed. This remains exploratory due to the exact-gate limitation
and cannot predict the compositor-only overlay effect.

## Live counterexample: 20:47 run

The user reported that cadence barely changed between settings and confirmed
Desktop Mode. The newest completed trace is
`/home/deck/moonlight-logs/moonlight-pacing-20260910-204716-1918948.vrrtrace`,
708721 bytes, last write 2026-09-11 01:51:41 UTC,
SHA-256 `dc8b46263224faef4f8020a5a4d45cfab0545a8df262467c4f3d250121338048`.
The matching app log retains four connection summaries: smoothed/Lowest 85.3%,
unsmoothed/Balanced 80.0%, unsmoothed/Lowest 79.9%, unsmoothed/Smoothest 81.4%.
All use Desktop Wayland, VAAPI HEVC, and Vulkan Mailbox, not Gamescope WSI.
Settings took effect. The user confirmed client Desktop Mode; the host workload
has not been independently identified as gameplay or desktop content.

To investigate the enabled smoother specifically, separately selected the first
archived connection from this same run:
`moonlight-pacing-20260910-204716-1918948-connection-1.vrrtrace`, 755487 bytes,
last write 2026-09-11 01:48:16 UTC,
SHA-256 `91021394a513ae0b11aecabe658b148ded918970f8b3ee4a4f2a2c2c0873989a`.
Fresh exact runs for both files exited 3: sequence integrity and reference
controller state matched, but strict native diagnostic validation still failed.
Outputs are `build/judder-204716-baseline.json` and
`build/judder-204716-smoothed-baseline.json`.

In that smoothed connection, excluding the first 50 presented frames to separate
startup leaves 3197 frames and 3195 adjacent interval pairs. Intended target jerk
exceeds 2 ms in 12 pairs (99.62% within threshold). After readiness clamps, 440
pairs fail (86.23%); at submission, 456 fail (85.73%). Of these 3197 frames, 963
have their target pushed later by more than 100 us; p99 clamp is 5.762 ms.
VAAPI synchronization waiting averages 7.907 ms in that steady subset, with
whole-connection p99 13.019 ms. These call durations include CPU wait/wakeup and
are not isolated GPU execution times. The trace identifies late readiness as
the first large loss of smoothed cadence, before compositor submission.

The earlier 99.4% counterfactual reused an older connection's recorded readiness
and does not establish that those completion times persist in a new session.
The new exact/reference replay reproduces the degraded cadence. More smoothing
strength cannot make an unavailable decoded frame meet an earlier deadline.
The current latency preset caps also constrain how much readiness variation
can be covered; the percentage is not evidence of a broken setting toggle.

FFmpeg already requests LOW_DELAY. Installed Mesa 26.1.7 additionally supports
`AMD_DEBUG=lowlatencydec`; matching driver source carries that flag into the VCN
decode command. Moonlight now requests it before graphics initialization, only
within its process, preserving existing flags and an explicit diagnostic opt-out.
This is a targeted candidate, not a demonstrated fix for the observed waits.
The next measurement must compare sync waits, readiness clamps and actual
submission cadence with smoothing held on. A full app relaunch is required.
See [Mesa's documented AMD option](https://docs.mesa3d.org/envvars.html#radeonsi-driver-environment-variables).

Decoder candidate delivery: incremental app and diagnostics builds passed,
as did the flag-preservation/idempotence regression and all four VRR suites
plus replay help. Offscreen startup smoke checks verified both the request log
and the explicit opt-out. The local wrapper forwards the diagnostic option and
AMD flag variables and passes `bash -n`. This validates configuration plumbing,
not physical decode latency. Executable SHA-256: `369915ef125fcd90616c169ea9eb489f5051c1b86bc36773d97b996b92525f8e`.

## Latest desktop run and Linux growth guard

Selected again from `/home/deck/moonlight-logs` immediately before analysis:
`moonlight-pacing-20260910-210132-1928809.vrrtrace`, 1,062,346 bytes,
2026-09-11 02:02:39.818182 UTC, SHA-256
`b9bc9c1f82e1f7cb16397255e8fa7fb42bd3536cfb27a61bf0b7b54bf9a6860b`.
The matching log reports smoothing off/Smoothest, 82.1% estimated motion
cadence, 100.0% client pacing, queued 3.79 ms, decode-sync wait 2.95 ms,
and selected buffer 15.96 ms. The process requested Mesa low-latency decode;
this is not evidence of a proven driver/visual improvement.

The trace loaded a version-18 predictive profile. Requested buffer averaged
19.86 ms and capacity limited it on 97.27% of presentations. Linux's pre-worker
VAAPI sync advances the scheduling readiness timestamp; Windows carries a
per-frame decode fence into GPU rendering and establishes render readiness
later. These boundaries are not interchangeable. Removing the Linux sync
without replacing its ownership/dependency contract would be unsafe.

The Linux-only growth guard now requires consecutive eligible output spacing
errors over 2 ms attributable to late decode/preparation readiness. Absolute
readiness predictions, cached tails, host spacing changes, and native blocking
alone cannot raise the requested buffer. Catch-up charges the delayed frame's
original buffer once. Version-19 history isolates these demands from old
predictive profiles; Windows keeps its current prediction rule. Details are in
architecture.md section 9.2.

Final diagnostic rebuild and all four required VRR suites plus replay help
passed. New regressions cover platform selection, cached-demand startup,
readiness-caused growth/release, source jitter, constant lateness, native-only
blocking, catch-up, dropped/ineligible frames, and replay parameter validation.
The final exact baseline exited 3: sequence integrity, reference targets,
submissions and controller diagnostics match, but strict Vulkan native
presentation validation remains unsupported. All replay conclusions below are
exploratory, not strict A/B proof.

Complete captured parameter snapshot plus only the new threshold produced
`build/buffer-210132-candidate.json`; final baseline is
`build/buffer-210132-final-baseline.json`. Median modeled buffer changed from
16.000 to 14.913 ms. Presented jerk above 2 ms stayed 175 per mille of 4,213
qualifying pairs. Both models retained 4,464 presentations and 22 drops.
This does not establish a smoothness improvement. The candidate latency
histogram contains 4,229 samples versus baseline 4,464, so its nominal latency
percentiles are not an equal-denominator end-to-end comparison.

The final five-scenario nominal/decision/preparation/submission/scheduler batch
passed all configured interval-safety and 30 ms p99 latency assertions, with
no saturated scenarios (`build/buffer-210132-stress.json`). Those checks share
the capture/model limitations above. Incremental application build and
offscreen help passed. Local executable SHA-256:
`e93134930a42e34b7c6f2453aaba68e1a200c8e11bd6ce869711d5aae77575ca`.
The existing Moonlight process was left running; the launcher uses the new
executable on the next full application launch. Neither desktop visual parity
nor the overlay-dependent Gaming Mode problem is established as fixed.

## Per-frame repaint follow-up

The user reports that the forced-composition trial still showed the problem.
The separate `composite_force` read failure and a nested Gamescope abort were
also observed; neither should be treated as the cause of the longstanding
performance-overlay dependency.

Added **Test Gamescope repaint after each frame**, default off. Reopen the
rebuilt native app, leave the composition checkbox off, enable the repaint
checkbox, and reconnect. Successful Vulkan video submissions notify a helper
that sends `debug_force_repaint` over a persistent private Wayland connection.
The helper coalesces notifications while awaiting command acknowledgement;
there is no process launch, socket roundtrip, or compositor wait on the render
thread. This asks for a base-plane repaint, not an overlay repaint, and may be
redundant with the normal surface commit. An acknowledgement is not scanout
proof. No live improvement is claimed.

The isolated fake-compositor test covers per-frame requests, idle behavior,
bounded outstanding requests, missing acknowledgements, rejected commands,
missing protocol, disconnected sockets, and teardown with an unresponsive peer.
It and all four required VRR suites passed, as did replay help, the incremental
application build, offscreen app help, and `git diff --check`.
The local launcher uses `build/app/moonlight`, SHA-256:
`e139ff3b9edffc046a38b990e51b47ee5252e5fbb412725d8a1ce66f2cc7f15d`.
No desktop/compositor/service restart or gaming session termination was performed.

Live repaint result: the user reports no meaningful improvement. The latest
completed app log, `moonlight-20260910-215651-1966426.log`, matches the executable
hash above and records 2,049 requests and 2,049 acknowledgements in the enabled
connection. This confirms the command path ran, not a visual remedy.
Both connections also report a 60 Hz presentation snapshot with 120 FPS
requested, disabling effective V-sync and Moonlight VRR. Gamescope WSI later
reports an 8.33 ms refresh cycle. The nested launch therefore did not establish
production VRR-path parity. Configure the nested advertised refresh explicitly
before comparing that path; do not attribute the longstanding overlay dependency
to this separate nested-session mismatch without evidence.
