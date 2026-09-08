# Conditional tearing protection experiment — 2026-09-08

The objective is to prevent tearing while leaving adaptive presentation enabled
when timing has sufficient stable headroom. The opt-in experiment restores
vrr12's protected native call and its matching spacing/mode policy together.
It retains current buffering, GPU readiness safeguards and feedback preservation.
It does not enable the separate cadence-smoothing experiment.

## Why CPU timing stopped being enough

The older native helper actually called `Present(0,0)` for protected frames.
vrr14/vrr15 call `Present(1,0)`, omit the software spacing floor for protected
frames and choose the mode per frame. Changing native queue behavior also
changes the relationship between the CPU call and the frame reaching the
display. Accurate CPU deadlines do not establish that this relationship stayed
the same. [DXGI documents different queue behavior for different sync intervals](https://learn.microsoft.com/en-us/windows/win32/api/dxgi/nf-dxgi-idxgiswapchain-present).

In the reporter's latest vrr15 capture, frame 318 (Present ID 260) submits
protected at 2,677,130 microseconds. Frame 319 submits with `ALLOW_TEARING` at
2,686,064 microseconds. The 8,934-microsecond CPU gap exceeds the nominal
6,944-microsecond display period, so the existing diagnostic calls it safe.
However, a later query associates the preceding protected frame with a native
refresh timestamp of 2,685,274 microseconds: only 790 microseconds before the
adaptive call. That query arrives after the call and cannot inform that decision.

This demonstrates a blind spot in the CPU-spacing classification. It is not an
optical measurement of a tear. Of 579 protected-to-adaptive transitions in this
capture, 263 calls are within one nominal display period after the preceding
protected frame's retrospectively matched native refresh timestamp. A native
refresh timestamp remains an interval observation, not an exact per-frame flip.

## Conditional policy

Set `MOONLIGHT_VRR_V12_PROTECTION=1` before launching the experimental executable.
The DXGI worker records the default-zero controller flag
`dxgi_vrr12_protection=1` and applies these presentation settings atomically:

| Condition or constraint | Behavior |
| --- | --- |
| Stable cadence with sufficient headroom | `Present(0, ALLOW_TEARING)` |
| Tight headroom or cadence instability | Protected `Present(0,0)` |
| Switching modes | Enter protection below 225 microseconds headroom; leave at 400 microseconds or more after the instability hold |
| Cadence instability | Require 64 clean frames before releasing its hold |
| Both modes | Enforce previous actual CPU submission plus one display period and guard |
| Buffering and readiness | Keep the existing policy and GPU completion checks |
| Smoothing/metronome | Disabled |
| Learned calibration | Separate experiment key and file |

Headroom is the controller's fitted cadence margin after its guard, not a
measured panel blanking interval. Borderline rates may stay protected. A clean
120 FPS source on a 144 Hz display has substantially more margin than 138 FPS
on the same display, so recovery tests distinguish them. The hold prevents
rapid mode switching; it is deliberately conservative after a discontinuity.
No new wait for a pending native acknowledgment can keep this policy locked.

Protected interval-zero frames must be attributed using actual native flags
as well as interval. The shared native-call helper supplies both DXGI arguments
and telemetry, and worker/replay tests exercise that attribution. Legacy
captures retain the old interpretation when the new flag is absent.

This restores a known presentation contract; it does not establish a new
physical tearing probability estimator or prove the reporter's display is fixed.
Protection can still have latency or throughput costs near the refresh ceiling.

## Playback evidence and limits

The selected latest capture is
`/Users/chasepayne/Downloads/moonlight-2.vrrtrace`, 1,374,569 bytes, modified
2026-09-08 21:03:46.609577 UTC, SHA-256
`9def6ec3587c5d55a6439b9c098d6cdf4d12d9c61166ec860de8f039e4d0e556`.
Its clean-close footer reports one dropped trace row: arrival sequence 3 is
missing. Strict exact replay fails, so candidate results are exploratory.

The explicitly compared vrr14 capture is
`/Users/chasepayne/Downloads/moonlight.vrrtrace`, 585,075 bytes, modified
2026-09-08 19:40:07.588817 UTC, SHA-256
`cb97adf2c181db43467e66060b3ed30c3ff25512fae3127d585da23dcf470aed`.
Its sequence is complete and the unchanged policy passes strict exact replay.
Both captures report 144 Hz and 144 FPS requested, with approximately 138 FPS
source cadence in gameplay. No vrr12 capture or 120 FPS session is available.

The portable simulator runs the real controller. It cannot reconstruct changed
DXGI queue service or GPU preparation costs from old captures. A candidate
changing the protected native contract is explicitly marked nonpredictive:
recorded native feedback is suppressed, and qualification refuses native or
latency proof. Saturated fixed-admission runs cannot establish either success
or failure of the native experiment.

Use the [standalone build](BUILDING-cmake.md), then run the control and native
contract experiment as a single batch after independent exact baselines:

```sh
python3 -B scripts/review-vrr-playback.py --replay build/vrr-cmake/vrrreplay --config tests/vrr/configs/vrr12-protection-review.json --output build/protection-review-1 /path/to/moonlight-2.vrrtrace
```

The output directory must be new. A failed strict baseline or scenario assertion
remains a nonzero process result; full exploratory evidence is retained. A
nonpredictive native-contract experiment must never be promoted to qualified
proof merely because its modeled interval violations are zero.

The final standalone build passes all 12 CTest entries on macOS, including the
30-case replay/config suite with no skips and 17 Python evidence-gate cases.
Controller regressions check startup, late CPU submission floors, post-hitch
recovery and a negotiated 144 FPS stream changing through 138, 136, 120 and
138 FPS. Native/worker regressions check actual call arguments, mode attribution,
default-off and non-DXGI behavior, and calibration isolation.

After that rebuild, fresh playback runs retain the vrr14 exact baseline (exit
0) and reject the incomplete vrr15 baseline (exit 3). Both compatibility
scenarios are marked native-contract-changed and nonpredictive, with qualification
denied. The review completed with verified input provenance and exit 3. A complete
live trace from the new contract is still needed to verify its full capture
exactness; synthetic native-call tests do not provide that evidence.

## Display validation

Use the same experimental executable for ordinary launch and opt-in launch,
with separate completed traces and matching Moonlight logs. Verify the recorded
flag and actual native arguments before comparing results. Exercise 120 FPS on
a 144 Hz display, 138 FPS on 144 Hz, and the separately reported 120 Hz mode.
Check visible tearing first, then protection entry/release, mode switching,
frame drops and latency. Existing traces cannot substitute for these sessions.
