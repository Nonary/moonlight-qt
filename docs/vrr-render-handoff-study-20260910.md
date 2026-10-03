# Linux decode and render handoff investigation — 2026-09-10

Pipeline improvements are worth pursuing across all three latency presets. The clearest next experiment is the existing Vulkan Video decoder and semaphore handoff, followed by isolating the VAAPI import cost. No production behavior or running session was changed by this investigation.

## Capture and fidelity

- Selected newest completed capture at analysis start: `/home/deck/moonlight-logs/moonlight-pacing-20260910-231235-2019015.vrrtrace`. The newer `233716-2032296` capture belonged to a running Moonlight process and was excluded.
- Length: 8,948,295 bytes. Modification time: 1789100449101675820 ns since epoch. SHA-256: `f8b33be04923472860d15bfedb62bcb9eb62903f3ba46b4ae2d12170862ed82d`.
- Complete recorded accounting: 37,261 arrivals, 37,229 presentations, 32 drops. Footer, decoded hash and sequence checks passed in the observed report. No matching launcher sidecar was available.
- Fresh strict replay returned exit code 3: `Original deadline diverged on frame 3925`, at the first oscillation switch. The fixed-policy replay cannot reproduce this oscillating capture exactly. All counterfactual results below are exploratory sensitivity tests, not exact A/B evidence.

## Observed client time

These stages share the same 37,229 presented-frame denominator. Client time is decoder output to present-call return; GPU render completion and physical scanout are not measured by these CPU boundaries.

| Stage | Mean ms |
|---|---:|
| Actual queue residence | 0.169 |
| VAAPI synchronization wait | 4.128 |
| Controller calculation | 0.025 |
| Wait before preparation | 2.305 |
| Preparation | 1.027 |
| Wait after preparation | 1.717 |
| Present call | 0.236 |
| Other CPU handoffs | 0.021 |
| Total client processing | 9.629 |

Preparation itself contains 0.083 ms swapchain acquisition, 0.931 ms mapping/render recording/unmapping, 0.0046 ms repeated decode synchronization, 0.0014 ms explicit flush, and small surrounding overhead. The repeated sync is therefore not the 4 ms problem. GPU work is already flushed before the target wait.

## Fixed-policy sensitivity simulation

The five inputs each preserve all arrivals and source timestamps. Each runs three fixed captured preset configurations for the entire capture; none represents the observed sequence of preset switches. Smoothing and buffer rules are unchanged within each preset comparison.

- Baseline uses captured GPU-ready timestamps and preparation service samples.
- Faster-readiness cases advance each recorded ready timestamp by 1 or 2 ms, clamped at pacer arrival. This assumes an actual improvement in readiness; removing a CPU wait without advancing GPU completion does not satisfy the assumption.
- Half-preparation cases halve recorded preparation and acquisition durations. This deliberately optimistic sensitivity includes startup costs; it is not proof that import caching or asynchronous rendering can achieve those costs.
- Service samples are replayed by completed-service ordinal; changing service timing can change admission and drop patterns. The model does not execute native GPU work, predict changed decoder backpressure, or measure scanout.

Latency below is pacer arrival to CPU submission, not the observed decoder-output-to-present-return total above.

| Hypothesis | Preset | Mean ms | Saved ms | p99 ms | Jerk >2 ms | Drops |
|---|---|---:|---:|---:|---:|---:|
| baseline | lowest | 8.593 | 0.000 | 12.640 | 18.68% | 31 |
| baseline | balanced | 11.764 | 0.000 | 17.903 | 18.25% | 31 |
| baseline | smoothest | 11.737 | 0.000 | 19.742 | 18.03% | 30 |
| half-prepare | lowest | 8.047 | 0.546 | 12.204 | 18.42% | 54 |
| half-prepare | balanced | 10.561 | 1.203 | 18.519 | 18.11% | 29 |
| half-prepare | smoothest | 11.176 | 0.561 | 19.263 | 18.01% | 25 |
| decode-faster-1000us | lowest | 7.734 | 0.859 | 11.640 | 18.61% | 31 |
| decode-faster-1000us | balanced | 9.819 | 1.945 | 16.746 | 18.13% | 31 |
| decode-faster-1000us | smoothest | 9.836 | 1.901 | 17.530 | 17.87% | 30 |
| decode-faster-2000us | lowest | 6.891 | 1.702 | 10.640 | 18.57% | 31 |
| decode-faster-2000us | balanced | 8.524 | 3.240 | 15.489 | 18.13% | 31 |
| decode-faster-2000us | smoothest | 8.149 | 3.588 | 15.498 | 18.03% | 30 |
| both-half-prepare-1000us | lowest | 7.192 | 1.401 | 11.204 | 18.41% | 54 |
| both-half-prepare-1000us | balanced | 9.391 | 2.373 | 17.217 | 18.07% | 29 |
| both-half-prepare-1000us | smoothest | 9.216 | 2.521 | 17.261 | 18.04% | 25 |

All six faster-readiness cases passed accounting, zero adaptive spacing violations, no additional drops and p99 latency no higher than their fixed-preset baseline. Jerk percentages use approximately 37,228 interval pairs per scenario, including source variation and modeled frame gaps. This is not verified display smoothness. Half-preparation increased Lowest-latency drops from 31 to 54; faster preparation alone is not a validated candidate.

## Concrete architectural experiments

1. **Native Vulkan decoding and GPU semaphore handoff.** `PlVkRenderer` already supports `AV_HWDEVICE_TYPE_VULKAN`. The installed libplacebo 7.360.1 `libav_internal.h` acquires Vulkan frames using their timeline semaphore/value, rather than the VAAPI path that maps through DRM PRIME. The session log confirms Vulkan rendering with a VAAPI decoder, so renderer selection alone does not activate this path. The local read-only Vulkan capability probe showed no video extensions by default; with process-local `RADV_EXPERIMENTAL=video_decode`, the same RADV VANGOGH device exposed video queues and H.264/H.265/AV1 decode extensions. This proves extension exposure only, not codec/profile correctness or a latency improvement. [Mesa documents the experimental flag](https://docs.mesa3d.org/envvars.html#radv-experimental). The deprecated `RADV_PERFTEST=video_decode` spelling also worked but emitted a deprecation warning.

   A controlled trial needs explicit Vulkan hardware-decoder selection, VAAPI fallback, and readiness diagnostics that retain actual GPU dependency timing. The current Vulkan frame path makes `waitForDecode()` return zero; treating that as zero GPU decode latency would be a measurement error and can undertrain the readiness budget. Benchmark the same codec/profile and source workload before adopting a backend change. No GPU decoding benchmark ran alongside the active stream.

2. **Overlap CPU preparation with outstanding decode.** Prepare commands and retain the decode dependency on the GPU instead of serializing all preparation behind `vaSyncSurface()`. The mean of per-frame `min(decode wait, preparation)` is 1.011 ms, an optimistic upper bound on hiding the recorded CPU preparation span with unchanged decoder work. Actual overlap will be smaller if part of preparation still depends on decoded data or swapchain availability. This cannot erase the entire 4.128 ms synchronization span. Scheduling must preserve the dependency and then learn the shorter critical path; returning from a CPU API earlier is insufficient.

3. **Measure and reuse surface imports where they dominate.** The measured 0.931 ms render span includes `pl_map_avframe_ex`, color/overlay setup, `pl_render_image`, and unmapping. Split these before attributing the span to shaders. If VAAPI surface import is expensive, cache imported image resources by surface and hardware-frame context, with explicit synchronization, per-frame lifetime protection, and invalidation on pool/context changes. Keeping AVFrame references forever would starve the decoder pool. This is a design candidate, not an implemented or measured cache.

4. **Separate GPU completion from CPU wake delay.** The 4.128 ms `vaSyncSurface` span includes remaining GPU work, driver synchronization, and scheduling delay. GPU completion/fence timing is needed to identify how much can be removed by a decoder/driver change and how much can be overlapped. The existing process-local AMD `lowlatencydec` request is already enabled, so setting the same flag again is not a new experiment.

## Reproduction artifacts

`build/render-handoff-study/make-study.py` creates compact synthetic CSV inputs and the explicit preset configuration from the selected decoded capture. `vulkan-capabilities.cpp` performs instance/device-extension enumeration only. `baseline.json`, the four candidate JSON files, `stages.json`, and `checks.json` retain results. Run the simulator inside `moonlight-dev`, which provides the matching FFmpeg runtime:

```sh
podman exec --user deck --workdir /home/deck/sources/moonlight-qt moonlight-dev \
  build/vrr-tests/vrr/vrrqueuesim build/render-handoff-study/decode-faster-1000us.csv \
  --config build/render-handoff-study/presets.json \
  --output build/render-handoff-study/decode-faster-1000us.json
```
