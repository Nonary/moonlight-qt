# Shared buffering service classification correction

This change follows the completed September 20 14:31:51 GPU diagnostic v3 run.
The latest completed capture is the low-bitrate final session; its preceding
connection is the separately identified high-bitrate session. Paths, sizes,
timestamps and hashes are recorded in `build/gpu-live-analysis-20260920/v3-summary.json`.
No parameter sweep or modeled performance claim is used here.

## Defects

The shared interval buffer's revision-1 service gate compared one late frame's
serial service with one intended frame interval. That is a test for a slow
frame, not sustained overload. A transient GPU dependency wait can exceed one
interval and recover on subsequent frames; the gate rejected protection for
that very stall. It could still grow for cheap catch-up frames, leaving it
stuck at insufficient protection. Both D3D and Vulkan use this observer.

The direct regression produces a 14 ms asynchronous readiness delay once every
four 10 ms source intervals, plus 0.5 ms serial preparation. It models one
worker's actual start time and holds to its presentation deadline. Revision 1
stalls at 4.5 ms protection; revision 2 reaches about 13.6 ms in this fixture.
A separate constant 11 ms serial workload against 10 ms source intervals
must remain at 1 ms, with no growth. These are deterministic mechanism tests,
not performance estimates for the live traces or proof of 99.5% scanout quality.

Deferred Windows GPU completion had two further attribution problems. Its
preparation-to-completion observation span includes intentional cadence hold,
yet was treated as serial service. Conversely, even a verified incomplete
fence at the final presentation boundary could not contribute readiness
lateness, because the preparation-complete timestamp was used instead.
The completed-before-wait flag also came from the earlier preparation poll,
not the first poll at the actual residual wait.

## Correction

Production selects `playout_serial_service_gate=2`. The gate compares measured
serial service and decoder-queue pressure with total intended time over the
same qualified one-second window used to measure interval pressure. Sequence
breaks clear that evidence. Sustained excess work still vetoes growth; fresh
readiness lateness, current pressure, quality history and the existing bounded
growth law are still required. Buffer caps, hold/release rates, frame queue
capacity, source mapping and scheduling targets are unchanged.

Deferred GPU service now counts preparation plus the actual residual CPU wait,
excluding the deliberate pacing hold. A fence observed incomplete at the final
wait contributes its eventual completion to readiness. An already-complete
fence checked after a late scheduler wake contributes no manufactured miss.
D3D reports its first poll at that final wait, including the exact fence value.
No GPU dependency or safety wait has been removed.

Revisions 0 and 1 retain historical behavior. The existing captured parameter
records revision 2; there is no new trace column or file schema. Replay changes
only accept the new revision and forward the existing captured pending/completed
evidence, so old recordings can still be checked exactly. There is no tuning of
replay costs or scenarios to claim an improvement.

Validation must include the transient/still-overloaded regressions, an actual
deferred wait versus a late observation, the existing deterministic suites,
historical exactness and the native application build. Windows backend source
needs a Windows build and live test; the Linux build cannot establish that.
Fresh live captures are required to determine how much this repairs the user's
symptom. The roughly 10 ms hardware/driver readiness wait is not eliminated by
this policy correction, nor is the separate first-frame CPU rendering stall.
