# Reporter playback review — 2026-09-08

The bounded smoothing experiment improves CPU submission cadence in these
captures, but does not establish that the reporter's jitter or tearing is fixed.
The proposed native presentation rollback remains unvalidated. Production
presentation, smoothing defaults, GPU fences and buffer ownership are unchanged.

The subsequent [conditional protection experiment](TEARING-REVIEW.md) addresses
tearing directly. The smoothing results below remain a separate scheduling
investigation and are not its acceptance criterion.

## Inputs and fidelity

The downloaded files were freshly enumerated, hashed and independently audited.
They are distinct captures of the same reporter's 4K HDR, HEVC Main10, 144 Hz
session configuration, with 144 FPS requested and approximately 138 FPS source
cadence during gameplay. No vrr12 capture or 120 FPS session was available.
There were no matching replay sidecars or mounted Windows trace shares here.

| Capture | Bytes | Modified UTC | SHA-256 |
| --- | ---: | --- | --- |
| `moonlight.vrrtrace` (vrr14 log) | 585075 | 2026-09-08 19:40:07.588817 | `cb97adf2c181db43467e66060b3ed30c3ff25512fae3127d585da23dcf470aed` |
| `moonlight-2.vrrtrace` (vrr15 log) | 1374569 | 2026-09-08 21:03:46.609577 | `9def6ec3587c5d55a6439b9c098d6cdf4d12d9c61166ec860de8f039e4d0e556` |

The vrr14 capture passes `--require-exact-baseline`, including all 2218 scheduled
decisions and 2212 submissions. Its 2226 allocated trace rows are complete.
This establishes deterministic reproduction of the captured policy on macOS.

The vrr15 capture has 5317 of 5318 allocated rows. Its clean-close footer and
payload digest agree, but arrival sequence 3 is missing. Strict replay returns
3 and both sequence integrity and baseline exactness are false. Its original
deadline and source mapping differ by 5919 microseconds on frame 38, consistent
with missing startup controller state; this is not a demonstrated replay bug.
Other fidelity discrepancies remain visible in the full report. All vrr15
candidate results are exploratory, even though later deadlines reproduce.

Replay previously aborted at that first original-deadline mismatch. It now
retains the full diagnostic summary and includes original-target drift in the
strict exactness gate. It never fills the missing trace row with invented data.

## Candidate and results

The selected scheduling experiment keeps current native presentation and
feedback preservation. It enables gain 350/1000 cadence smoothing, period
tracking alpha 50/1000, and at most 2 ms positive smoothing lag. The opt-in
`playout_native_hitch_smoothed_reference=1` judges native intervals against
source time plus intentional smoothing, so smoothing itself cannot demand
additional buffering. This flag defaults to zero and remains zero in production.

The lag allowance shares the existing queue-capacity budget. The selected 2 ms
allowance does not trigger capacity clipping in either nominal capture. Larger
6 ms allowances do clip the budget in some frames, so their lower delay is not
treated as a free improvement. Both raw source fidelity and displayed-cadence
proxies are reported.

The following are whole-capture CPU submission metrics. They do not measure
physical scanout, and the two capture rows are evaluated independently.

| Capture | Jerk >2 ms, session → candidate | Jerk p99 ms | Decode-to-submission mean ms | Decode-to-submission p99 ms |
| --- | ---: | ---: | ---: | ---: |
| vrr14, exact baseline | 22.3% → 13.8% | 3.786 → 3.696 | 11.978 → 11.941 | 17.594 → 17.613 |
| vrr15, exploratory | 19.4% → 12.5% | 3.658 → 3.640 | 11.975 → 11.649 | 18.194 → 17.965 |

The frequency of uneven intervals improves, but p99 barely changes. Raw-source
spacing residual p99 worsens from 2.950 to 3.330 ms on vrr14 and from 3.093 to
3.123 ms on vrr15. The worst tail is not eliminated. This supports a bounded
experiment, not a declaration that the reported problem is solved.

Restoring minimum submission spacing, with or without old mode hysteresis,
saturates the fixed-admission replay worker. Its apparently better cadence is
not usable evidence. Conversely, saturation cannot establish that the actual
vrr12 native path would fail: this replay does not change the captured GPU and
presentation service costs when it changes controller settings.

The versioned stress config compares this candidate and the current policy
under nominal execution, periodic decision/preparation/submission delays and
scheduler bursts. All 20 scenarios across the two captures pass the explicit
model checks: no interval violations, p99 decode-to-submission at most 30 ms,
padding at most 16 ms, and no worker saturation. Candidate p99 latency stays
below 25.6 ms in these injected scenarios. These are fault-test results, not
normal operating latency or proof of optical smoothness.

## What happened to timing-based prediction

Prediction does not inherently require a displayed timestamp for every frame.
It does require a stable relationship between GPU readiness, submission and
native presentation behavior. Between vrr12 and vrr14, production disabled
cadence smoothing, replaced protected `Present(0,0)` with `Present(1,0)`, removed
the corresponding software minimum spacing, and switched to per-frame mode
selection. Those changes require the old timing assumptions to be validated
again. The vrr15 feedback fix preserves learned identities across transitions;
it does not restore vrr12's scheduling or native presentation behavior.

The reporter's misses develop mainly during preparation/GPU readiness. A
precise final CPU handoff cannot recover time already lost before that handoff.
More conservative scheduling could absorb some variation, but no vrr12 capture
is available to establish which old behavior supplied the improvement here.

DXGI refresh timestamps remain interval observations. An offline identity audit
can find usable anchors for 1938/1945 synchronized submissions but only 7/267
adaptive submissions in vrr14; vrr15 has 4653/4667 and 10/620 respectively.
An anchor before a frame's submission is not that frame's flip timestamp.
Aggregate coverage therefore hides most adaptive outcomes. Neither capture
enabled raster sampling. Smoothing also changes native sample eligibility, so
unchanged native miss counts cannot be interpreted as stronger coverage.

The simulator retains captured preparation durations and shifts captured native
service latency with candidate submissions. It does not model how changing
native Present arguments changes queue replacement, compositor service or GPU
waits. This limits proof of the coupled native rollback, rather than imposing
a requirement to optically measure every frame in normal operation.

## Reproduction and validation

Use [BUILDING-cmake.md](BUILDING-cmake.md) to compile the actual C++ controller,
replay and tests without the Moonlight application. The CMake entry supports
desktop toolchains; this review executed it on macOS with real Qt, SDL and
FFmpeg dependencies. Windows/Linux CMake execution was not performed here.

```sh
python3 -B scripts/review-vrr-playback.py --replay build/vrr-cmake/vrrreplay --output build/hybrid-review-1 --stress-config tests/vrr/configs/hybrid-scheduling-stress.json /path/to/moonlight.vrrtrace /path/to/moonlight-2.vrrtrace
```

The runner records binary/config/input hashes and process exits, uses a fresh
strict baseline for each capture, runs each scenario batch through replay's
own concurrency, and writes Markdown plus JSON with all qualification limits.
An incomplete capture returns a nonzero result while preserving exploratory
evidence. The inputs, baseline and candidate outputs remain separate.

All 12 CTest entries passed on the final source: nine shared C++ suites, both
utility help checks and the Python evidence-gate suite. The replay/config suite
ran 29 cases with no skips, including the real CLI divergence regression; the
Python suite ran 16 cases. The final candidate/stress runs were repeated after
that build, with input and binary provenance verified. No application package
was published or installed as part of this review.
