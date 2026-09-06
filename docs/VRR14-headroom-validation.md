# Current-FPS scheduling headroom

Implemented September 6, 2026. Controller 17 / cache payload 12 replaces the
controller 16 policy documented in `VRR14-buffer-calibration-validation.md`.
Older cached profiles are rejected.

The five-minute histogram retains raw excess receiver/pipeline readiness errors.
Available recovery headroom is calculated from the current source period, display
minimum interval and typical preparation time:

```
headroom = max(0, source period - max(display minimum interval, typical work))
queue target = max(0, p99.95(raw excess error) - headroom - 3 ms)
miss = raw excess error > applied queue buffer + original frame headroom + 3 ms
```

Each delayed GPU result and staged workload episode uses its original headroom
for scoring. The queue target uses current headroom. A change in content FPS can
therefore change the requested reserve without waiting for cached or live raw
samples to expire. Headroom is never deducted from the virtual work queues, and
is excluded from proven cached queue values. Both decisions prevent counting the
same recovery time twice. Game cadence remains excluded from receiver error.

Cold startup now subtracts available headroom from the negotiated source interval.
It no longer allocates twice the queue delay for 30 FPS as for 60 FPS on the same
display with the same preparation cost. Release is scaled by elapsed arrival time
in display intervals rather than by source frame count. A 32-observation minimum
and two-second evidence gate replace the 128-observation minimum; five-minute
retention and the empirical p99.95 calculation are unchanged. Recovery gaps do not
permit large drain steps. The 0.5 ms floor and 50 us timing guard remain.

## Validation

The Linux application build and installed-binary version smoke passed. Timing
regressions passed in both the normal build and ASan/UBSan build; cache, worker,
lab integration and deadline-audit tests passed.

Timing regression coverage includes 30/60/75/100/120 source FPS against
60/90/144 Hz display limits, raw-tail preservation across 120→30→120 FPS changes,
temporary work bursts, sustained overload, exact 3 ms boundaries after headroom,
and checkpoint restore with a pending workload episode. Rational RTP conversion
may move a tail by one 250 us histogram bin; comparisons allow that resolution.
Cache tests verify that recovery headroom cannot become proven queued delay.
Worker tests include native trace replay with zero divergences. Lab tests cover
recorder overwrite, fixed-refresh resimulation, missing feedback and concurrent
repeatability. The native presentation deadline audit remains unchanged.

Controlled simulations used a 144 Hz variable-refresh display model, no injected
compositor, feedback, scheduling or stall delay, and two workloads. Cold-start
measurements use approximately 1–1.25 ms preparation and zero receiver jitter.
The 310-second jitter workload uses 0–6 ms network jitter plus 0–1.5 ms assembly/
decode variation, and approximately 4–5 ms preparation. These are simulated
workloads, not measurements of an actual stream.

| Source FPS | Cold queue (ms) | Seconds to 0.5 ms floor | Jitter-work final target (ms) | Jitter-work decode-to-presentation p95 (ms) | Pipeline coverage |
| --- | --- | --- | --- | --- | --- |
| 30 | 6.994 | 4.400 | 0.000 | 4.965 | 100.000% |
| 60 | 6.994 | 4.300 | 0.000 | 4.966 | 100.000% |
| 75 | 6.994 | 4.293 | 0.000 | 4.966 | 100.000% |
| 100 | 6.994 | 4.270 | 1.444 | 5.992 | 99.970% |
| 120 | 6.994 | 4.267 | 3.111 | 7.652 | 99.975% |

All ten 310-second runs had zero drops. With clean input, whole-run
p95 decode-to-presentation latency was 1.638–1.642 ms across all five source rates.
The old 30 FPS policy took about 59 seconds to drain its cold-start allowance;
the current cold run reached the floor in 4.4 seconds.

Pipeline coverage measures recoverability within headroom plus the queue buffer
and 3 ms tolerance. Actual presentation error against the original feasible
plan remains separately measured with its existing 3 ms threshold. Passing the
recovery model does not establish that every frame met its original display
deadline, nor prove live smoothness or input-to-photon latency.

Artifacts: `build-tests/vrr14/headroom/clean-rates.json`,
`five-minute-rates.json`, bounded clean captures, and `deployed.json` with the
installed binary hash and rollback path. A running Moonlight process keeps its
existing executable; the desktop shortcut loads the replacement on next launch.
