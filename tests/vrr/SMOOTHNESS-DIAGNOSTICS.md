# Smoothness diagnostics and perceptual calibration

`scripts/vrr_smoothness.py` adds offline diagnostics without changing the
renderer, pacing policy, native binary, or the ongoing shared-deadline trial.
It reads existing compressed VRR traces or expanded CSVs. Python's standard
library is sufficient.

The objective includes small, repeated disturbances, not just large hitches:

- Absolute timing bands at 125, 250, 500, 1,000, 2,000, 4,000 and 8,000 us.
- Relative bands at 0.5%, 1%, 2%, 5%, 10%, 25% and 50% of source frame duration.
- Output interval changes and alternating short/long residuals, including
  patterns entirely below one millisecond.
- Source cadence deviations and client interval distortion reported separately.
- Missing source frame numbers, local drop rows, and reported gap fills.
- Clustered episodes, affected-interval fractions and episode spans. Continuous
  micro-judder can be one long episode; a low episode count is not sufficient
  evidence of smoothness.
- One-second windows with timing distributions, GPU readiness observation lag,
  CPU preparation cost and preparation deadline lateness.
- Seconds with no updates remain in the report. The end-of-window duration
  since the last known update is measured against the last known source period.
  Large pauses must not disappear merely because too few frames were delivered.

Every timing band is a diagnostic band, **not a human visibility threshold**.
Alternation counts include arbitrarily small residuals; assess their magnitude
and measurement uncertainty together. CPU clock accuracy is not inferred from
integer-microsecond timestamp formatting.

## Analyze a completed recording

```sh
python3 scripts/vrr_smoothness.py analyze capture.vrrtrace \
  --output smoothness.json --review-template review.json
```

The capture footer, decoded SHA-256, row accounting and arrival-sequence
completeness are checked. Incomplete captures may be inspected diagnostically,
but cannot train or run the forecasting model. Rebase boundaries, invalid RTP
intervals and missing presentation feedback are not silently joined.

Current Vulkan traces produce `timing_basis: cpu_submission_proxy`. This says
when Moonlight submitted frames, not when photons changed on the panel. The
existing DXGI `latch_time_us` is deliberately not treated as a frame-specific
display timestamp. The analyzer never fabricates visible-stutter probabilities
from CPU timing or from a preparation miss percentage.

Client interval distortion is the difference between the observed output
interval and the corresponding RTP interval. It can be introduced intentionally
by smoothing noisy sender stamps, so it is not automatically a defect.
Output cadence deviation compares output against the controller's fitted source
period, accounting for skipped frame numbers. Genuine game-rate motion and
imperfect cadence estimates can affect this metric too. Neither is ground truth.

Episode spans join threshold crossings less than 100 ms apart within the same
epoch. They include the intervening time; an isolated crossing has zero span.
These spans are not the duration of a physical screen freeze. Denominators and
eligible durations are included alongside all rates.

## Actual presentation feedback

A synchronized compositor or optical measurement can be supplied separately:

```json
{
  "trace_sha256": "the hash from smoothness.json",
  "basis": "optical",
  "trace_clock_offset_us": 0,
  "uncertainty_us": 100,
  "frames": [
    {"arrival_sequence": 12, "display_time_us": 123456789}
  ]
}
```

Use `--display-feedback feedback.json`. `trace_clock_offset_us` converts the
measurement clock into the trace clock and must come from synchronization;
zero is not a default assumption. Each point must identify a submitted frame.
Unknown/duplicate identities are rejected, and gaps in feedback break interval
comparisons. Incomplete feedback cannot train a model. Bands below twice the
supplied timestamp uncertainty need particular caution.

This importer is ready for measurements; it does not add compositor queries or
optical capture to Moonlight. Backend instrumentation and synchronized footage
are still needed to establish actual displayed timing and perceptual labels.

## Review and calibration

The review template proposes up to 60 uniformly selected windows. It contains
no invented negative labels. Review synchronized gameplay footage and add only
observed judgments, including subtle but noticeable judder:

```json
{
  "report": "smoothness.json",
  "trace_sha256": "the matching trace hash",
  "method": "observer_review",
  "context_id": "device/display/stream settings/pacing policy",
  "sampling": "uniform_windows",
  "reviewed_windows": [
    {"index": 4, "noticeable": false},
    {"index": 9, "noticeable": true}
  ]
}
```

Supported review methods are `observer_review` and `optical_review`. Sampling
must be declared as `uniform_windows` or `all_windows`. These declarations are
reviewer assertions, not proof that the review was conducted correctly. Review
the uniformly selected sample completely; choosing only striking bad moments
would bias probability estimates. Unreviewed windows remain absent, not false.
Optical timing alone does not establish whether a particular person noticed it.

Train and validate using **different complete captures** in the same hardware,
stream/pacing context and timing basis:

```sh
python3 scripts/vrr_smoothness.py calibrate \
  --train training-review.json \
  --validate separate-session-review.json \
  --output experimental-model.json
```

The initial predictor is deliberately simple: it bins the previous second's
relative timing RMS and its end-of-window no-update excess. It estimates the
chance of **any noticeable smoothness disturbance during the next second** from
independent labels, with smoothed empirical bin frequencies. It does not predict
the same window whose timing errors it has already observed. Empty target
windows remain eligible outcomes, avoiding an outcome-dependent exclusion of
large stalls. A sudden isolated disturbance with no prior signal may be
unpredictable by this persistence-oriented model.

The output reports held-out Brier score, the constant-training-prevalence
baseline, reliability by bin, sample counts and independent-window Wilson
intervals. Events cluster, so those intervals can be too narrow; more independent
sessions and session-level uncertainty estimation are needed before deployment.
Bins unseen in training yield no estimate. Different contexts are rejected.
No model is automatically approved for selecting smoothness profiles, even if
it beats the baseline on a small validation set.

To generate explicitly experimental forecasts:

```sh
python3 scripts/vrr_smoothness.py analyze another-capture.vrrtrace \
  --model experimental-model.json --context-id 'the matching context' \
  --output forecast.json
```

The forecast is a probability for a one-second window, not a per-frame failure
rate, episode rate, guarantee, or universal measure of perception. Motion
content, individual sensitivity and actual display behavior remain necessary
calibration considerations. CPU-proxy models can learn correlations with
observer labels, but do not become physical display measurements by doing so.

## Validation performed

```sh
python3 tests/vrr/test_smoothness_diagnostics.py
```

Tests cover steady cadence; repeated 400 us interval changes; source-only
judder; clustered disturbances; frame skips and resets; RTP wrap; missing or
mismatched display feedback; compressed chunk boundaries and corrupt captures;
seconds with no updates; future-label leakage; train/validation overlap; context
mismatch; and refusing unreviewed or deliberately selected bad-window labels.
Synthetic calibration tests establish implementation behavior, not real
predictive accuracy.

The preserved `1446` capture was analyzed successfully: all 43,040 trace rows
pass the footer/hash/sequence checks. Its diagnostic report and blank review
template are in `build-tests/vrr/smoothness-diagnostics/`. No real perceptual
model has been trained; reviewed footage is the remaining calibration input.
