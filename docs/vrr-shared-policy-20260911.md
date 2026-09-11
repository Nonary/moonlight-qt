# Shared Linux/Windows buffer policy — 2026-09-11

Linux live session setup now selects the existing Windows prediction-based buffer policy (history 18, 3 ms prediction margin inside preset caps). Historical Linux event-based parameters remain available to exact replay. Linux cache keys are separated from retired policy history. Current-policy replay and queue simulation select the shared policy on every backend.

The existing overlay queue statistic excludes the explicit worker GPU decode wait. No new stat is exposed; the existing CPU decoding statistic is unchanged. Internal full-client and decode-wait diagnostics remain available. Re-reporting the latest capture changes displayed queue from 9.924 ms to 4.919 ms without changing actual latency.

## Validation

- Linux application build and offscreen version smoke passed. This is not live rendering validation.
- Timing-controller, rate-policy, pacing-worker, and replay-config tests passed; replay help passed.
- Ten Python latency-report tests passed, including decode-wait accounting.
- The new worker regression checks a 10 ms internal frame interval with 4 ms decode wait and 1 ms rendering reports 5 ms queue.
- All 24 simulation cases completed. Twelve shared-policy cases retained accounting, zero adaptive-spacing violations and no additional drops; p99 stayed within the matched old policy plus the 3 ms margin.

## Capture and limits

Selected newest completed capture: `/home/deck/moonlight-logs/moonlight-pacing-20260910-233716-2032296.vrrtrace`; 4,452,504 bytes; mtime 1789101687540544747 ns; SHA-256 `27de32433e4021a5f4e6162ea5d15b7420c24060c5c7ca20cce097cb3e0be49a`. All 17,818 arrivals are accounted for and the decoded footer hash is valid. No matching launcher sidecar was available.

Fresh final strict replay exited 3. All 17,778 controller targets, decision state and diagnostics match, but only 17,758 of 17,762 submitted timestamps match exactly. This capture is exploratory, not strict A/B proof. The gate was not relaxed.

The comparison fixes each preset over the same arrival stream and replays recorded service costs; it does not measure changed GPU execution or physical scanout. The nominal source was a Lowest-latency capture; Balanced and Smoothest rows are modeled counterfactuals. All metrics below include startup. Latency is pacer arrival to CPU submission, not displayed queue delay.

| Scenario | Mean latency ms | p99 ms | Submission jerk >2 ms | Drops |
|---|---:|---:|---:|---:|
| lowest/old-linux/nominal | 11.053 | 14.182 | 20.73% | 55 |
| lowest/shared/nominal | 11.053 | 14.182 | 20.73% | 55 |
| balanced/old-linux/nominal | 16.562 | 18.408 | 17.75% | 54 |
| balanced/shared/nominal | 17.231 | 19.741 | 18.03% | 54 |
| smoothest/old-linux/nominal | 17.698 | 21.172 | 17.24% | 53 |
| smoothest/shared/nominal | 20.354 | 21.272 | 17.19% | 53 |

The shared policy does not lower Lowest latency on this capture: both policies remain cap-limited. Balanced gains 0.669 ms and Smoothest 2.656 ms of modeled mean latency; these are costs, not improvements. Sharing policy resolves platform behavior divergence but does not establish better latency or repair all history-retention behavior.

Stress scenarios inject 3 ms on three decisions every 300 decisions, separately at render wake, preparation and submission. Config, outputs, exact-gate evidence, test logs and explicit bounds are in `build/latest-queue-review/`. The Linux `moonlight-dev` launcher uses `build/app/moonlight`; no Windows release/share update was made.
