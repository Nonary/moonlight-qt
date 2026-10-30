# Historical master VRR notes

These notes describe the former master policy and are not the active cleanup policy.

VRR session cleanup (2026-10-29): current live sessions always use the
existing adaptive buffer, corrected input accounting and bounded catch-up.
The three former environment switches are ignored with a warning; historical
controller parameters remain available for replay. The trace writer is housed
in `vrrpacingtrace.cpp`. The production hitch-budget policy is unchanged.

Input lifecycle and requirement coordinates (2026-10-25,
`playout_input_stable_adaptation=14`): revision 13 held seeded protection when
no hitch-budget estimate existed, even after history expiry. Revision 14 keeps
the pre-expiry hold and then releases the obsolete seed through the existing
bounded slew. Growth uses the documented normal rise rate; proportional
slowing applies to release, rather than prolonging newly proven misses.

With a validated normalized producer mapping, the decoder handoff baseline is
the smallest qualified median. Later queue growth cannot become a new handoff
cost and manufacture upstream lateness (up to 12 ms in the mapping factorial).
The steady measured handoff remains covered; raw mappings retain policy 11's
rolling median. A connection that begins with sustained backlog has no timing
proof distinguishing that backlog from a steady handoff, so this baseline is
still an estimate rather than independent stage instrumentation.

The hitch-budget history retains signed requirements in the continuous,
drift-corrected producer coordinate and projects them into the current mapping
at query time. A later source mapping no longer leaves an old fast-rate need
as added buffer delay. Rate-drop release restores the existing tolerance-bounded
per-fresh-frame drain while the mapping settles. Revision 13 and earlier keep
their original arithmetic for exact replay.

Revision 14 also qualifies and renews ceiling protection whenever the current
frame's readiness proves it late. Requiring another phase increase failed to
renew equally late recurring frames, or to qualify a first late frame while
cold-start phase was already higher. The trajectory repays carry against the
current nominal target, so a one-microsecond buffer release does not create
one microsecond of fresh recovery. Earlier revisions retain their old bases.

Known short-send frames no longer establish the full-frame source-mapping
minimum under revision 14: normalizing their last-packet span still leaves an
earlier first packet, which otherwise moves the minimum ahead of later full
frames. First-frame initialization remains valid, and a sustained smaller
scene becomes typical through the existing 64-frame span window. Physical
receive-clock observations are unchanged.

Hitch-budget target (2026-10-22, `playout_input_stable_adaptation=13`):
revisions 8--12 drive the buffer toward one of two means, the breakthrough
hitch mean when failing and the all-disturbance mean when met, gated in
11/12 by a Poisson band. Both means lie far from the buffer that meets the
goal, so behind the window's lag the buffer overshot both ways: in the 16:53
capture (99.50% / 120 s, revision 12) it cycled 1.2 <-> 4.8 ms. Revision 13
has one target. The coverage history keeps every eligible outcome's needed
protection (excess over tolerance on the unbuffered slot basis) in an exact
rolling order index and counts breakthroughs (need above the buffer applied
then). For a candidate buffer B, frames needing more than B would have broken
through; the window's hitches per breakthrough (at least 10 breakthroughs,
floor 0.05, otherwise 1) converts that to predicted hitches. The target is
the smallest B whose predicted hitches fit the allowance less half its
Poisson deviation (eligible x allowance - sqrt(that)/2). It does not depend on
the applied buffer and moves only as outcomes age in or out; computed offline
on the 16:53 capture it drifted 1.3--4.0 ms in steps of tens to hundreds of
microseconds while revision 12 swung 1.2 <-> 4.8 ms. The existing
proportional, rate-limited actuator approaches it in both directions;
lost-cause outcomes still leave after 10 s. Revision 12 replays unchanged.

Goal deadband (2026-10-22, `playout_input_stable_adaptation=11`): revisions
8--10 switch the target between the breakthrough-hitch mean (goal failing) and
the disturbance mean (goal met). With coverage sitting on the goal, single
misses arriving or expiring flipped the verdict. In the 16:28 capture (99.50%
/ 120 s) coverage stayed at 99.48--99.53% while the target alternated
~1.7 <-> ~6 ms and the buffer swung 1.8 -> 5.6 -> 3.3 -> 5.1 -> 1.8 ms.
Revision 11 treats window misses as Poisson about the allowed count
(eligible x allowance) and holds the applied buffer while the count is within
one standard deviation (at least one miss) of it; it grows toward the hitch
mean only above that band and releases (including revision 9's
surplus-proportional step) only below it. Revision 10 replays unchanged.
Revision 12 makes the band lopsided: growth starts at half a standard
deviation above the allowed count, release still at a full one below. In the
16:36 capture revision 11 rested at 99.40--99.48% against a 99.50% goal;
the lopsided band rests at or slightly above the goal with the same
hysteresis against single-miss flips. Revision 11 replays unchanged.

Lost-cause spans (2026-10-22, `playout_input_stable_adaptation=10`): sizing is
unchanged from revision 9, but the session coverage history classifies each
eligible outcome against the last second of eligible outcomes. When at least
20 outcomes are present and misses reach four times the profile allowance
(at least 10%), the second is a lost-cause span: the client is failing
wholesale (warm-up, overload), not showing a tail a buffer trims. Its outcomes,
including the second that revealed it, move to a 10 s retention store. They
still count toward coverage and the hitch/disturbance means, so the buffer can
grow in response, but they leave the history after 10 s instead of the
selected history (300 s). The same span is removed from the client timing
score as an unscored gap. In the 15:48 capture the first ~20 s ran 10--22%
misses per second; 197 such misses held the 300 s goal failing until ~45 s and
kept the target alternating between the hitch mean (~6 ms) and disturbance
mean (~2 ms) for minutes. A level shift that a larger buffer can fix keeps
producing misses, so it stays in coverage and still drives growth. The
overlay coverage line shows retained lost-cause outcomes. Revision 9 replays
unchanged.

Startup render-prediction lock (2026-10-22, `preparation_initial_sample_excluded=2`):
with prediction enabled the target is at least now + typicalRenderUs(), the
median of learned preparation durations (first sample excluded). Warm-up
preparations of 23--27 ms left medians of two samples near 27 ms, so every
target landed past the stale horizon (two source periods), each frame was
dropped after its render wait without preparing, and no further sample could
lower the median: 6 of 9 connection starts from 15:27 froze at ~0.6 FPS with
~99% client-pacing drops (earlier starts had 15 ms warm-ups and escaped).
Value 2 uses the bounded render lead until eight preparations exist and caps
the prediction at one source/display period. Value 1 replays unchanged.

Expiring ceiling phase (2026-10-22, ceiling replacement policy 3): at the
display ceiling (source period within 0.1% of the display period) policy 2
kept the on-screen phase late input forced, up to a refresh, as protection.
That earned phase was a session high-water mark, reset only by a controller
reset. In the 15:10 capture (1440p AV1, 120 FPS on 120 Hz, rev-9 input buffer)
the buffer released 7.8 -> 1.0 ms, standing-phase replacements stopped after
36 s, and trajectory carry rose to 4--6 ms for the remaining ~200 s because
each late frame raised the threshold the standing rule must exceed. Policy 3
holds earned phase for 5 s after the late frame that last raised it, then
decays it at the interval tolerance per source frame; the phase trajectory
also repays carry at up to the full tolerance per successor (instead of 1/32
of the carry), still bounded by the panel's fastest refresh; a new late frame raises it from the decayed value. Late
frames recurring within the hold keep the protection (no skip per late
frame); otherwise the existing 32-frame standing rule sheds the phase with one
proven-ready replacement. Policy 2 remains replayable unchanged.

Policy 3 also changes the trajectory's ownership floor while presentation is
latched. The floor was the latched spacing anchor, max(previous submission
boundary, anchor + display period), less the smallest recent overshoot. A max
chain minus a jittering per-frame lateness ratchets later on every drop in
lateness. In the 15:27 and 15:40 captures (1440p AV1, host mean 8334.1 us on
8333 us, submission-boundary lateness 280--600 us) targets were spaced ~8342
us, phase climbed ~1 ms/s to 4--5 ms and a replacement shed it every ~5--6 s.
Latched presents cannot flip before the panel allows, so policy 3 floors a
latched frame continuous with its predecessor at the trajectory's committed
anchor (the previous plan, or its actual submission less the usual overshoot
when genuinely late beyond the allowance) plus one display period. Unlatched
and discontinuous frames keep the original floor.

Met-goal release (2026-10-22, revision-8 follow-up): new adaptive sessions
select `playout_input_stable_adaptation=9`. Revision 8 targeted
`min(disturbance mean + reserve, applied)` while the goal was met, so whenever
that demand was at or above the applied buffer the target equalled it and the
buffer froze: growth was refused by the met goal and release by the demand.
Connection 19 of the 2026-10-22 11:50 session (1440p AV1, 94.95% / 300 s)
held 1.928 ms for about 18 minutes at roughly 97.5% coverage. Revision 9 keeps
revision 8's targets and additionally, while the goal is met, steps the target
below the applied buffer by the fall rate times a history/30 time constant
(1--10 s) times the unused fraction of the miss allowance. The buffer therefore
keeps releasing until coverage approaches the goal; growth still requires a
failing goal. Revision 8 remains replayable unchanged.

The overlay's input coverage line now reports inputs excluded from scoring in
the short reporting window (over the buffer maximum or without valid timing).
In the same session, 4K 4:4:4 10-bit PyroWave connections 7 and 9 ran with a
standing one-frame pacing queue: decoder output to dequeue about 10 ms, then a
6.5 ms GPU completion wait, against 0.5 ms and 4.7 ms in connection 5 with the
same format. Required protection of 8--19 ms exceeded the ~4.2 ms buffer
maximum, so nearly every input was excluded; coverage stopped changing and the
lateness appeared as retiming (phase trajectory carry, 7--10 ms medians). This
matches the known GPU capacity limit for that format near 120 FPS; intervals
with ~104 FPS source cadence returned to ~8 ms decode latency. It is not a
buffer sizing defect.

Disturbance-mean release (2026-10-21, revision-7 follow-up): new adaptive
sessions select `playout_input_stable_adaptation=8`. Revision 7 averaged only
breakthrough hitches, and a breakthrough by definition required more than the
applied buffer, so that mean never fell below the buffer while any miss stayed
in the window. The capture after its install held 4.011 ms for about 85 s for that reason,
although the counted events stretched intervals by under 1 ms.

Revision 8 keeps the same per-event requirement (R minus tolerance on the
unbuffered basis) and the same reserve, but chooses the averaged set by
coverage. While selected-history coverage fails the goal, the target is the
breakthrough-hitch mean plus reserve and may only rise. While the goal is met,
the target is the mean over every disturbance, absorbed or missed, plus
reserve, and may only fall. Membership of that set does not depend on the
applied buffer, so release is no longer pinned by retained misses. Late inputs
that VRR room hid (neither absorbed nor a qualifying hitch) stay excluded.
Revision 7 remains replayable unchanged.

Rolling mean hitch sizing (2026-10-21, source baseline `b35ce457` plus
revision-6 follow-up): new adaptive sessions select
`playout_input_stable_adaptation=7`. This supersedes revision 6's retained
maximum target and score-driven release speed. That policy kept about 13.1 ms
in the 21:42 capture through a long clean span because its historical 120 s
coverage stayed below goal, even with recent coverage at 100%.

The controller now averages the minimum protection actually required by
eligible, qualifying input hitches over the user's selected history, using the
same session-wide history as coverage. Clean outcomes, invalid input, copied
rate-layer seeds and arbitrary raw readiness do not become hitch-size samples.
Each measured requirement is on the original unbuffered input-slot basis:
R minus the tolerated excess, not a residual blindly added to the current
buffer. The target is that arithmetic mean plus one tolerance reserve, bounded
by the existing minimum/maximum. Thus a 3 ms mean requirement and 1 ms tolerance
request 4 ms. Requirements expire individually by observation time. No hitch
samples request only the tolerance reserve, subject to the normal minimum.

Coverage can refuse an upward target while the selected goal is already met;
it does not multiply buffer demand, set release speed, or freeze release toward
a lower measured requirement because old outcomes remain below goal. Both
upward and downward movement converge proportionally, with the existing rise
and fall ceilings and a history/30 time constant bounded to 1--10 s. Learning,
saved layers, headroom-loss restoration and pending-release shortcuts cannot
jump the input buffer. Rate-layer changes preserve applied protection and use
the session estimate; the T-per-frame layer drain is disabled for revision 7.
Physical readiness, queue capacity, source cadence and trajectory recovery stay
separate and may affect measured total latency independently.

Session-history sums/counts update incrementally; there is no per-frame scan
of the full history. The session log explicitly reports the hitch mean, reserve
and sample count. The 10 s / selected-history percentages remain actual coverage
outcomes, distinct from the CPU Client timing severity score. An arithmetic
mean plus reserve is a sizing policy, not a mathematical guarantee about all
tail events. Historical revisions 0--6 remain unchanged for exact replay; the
existing revision-based calibration identity distinguishes revision 7. GPU
collection/readiness measurement is unchanged by this sizing correction.

Hitch-governed input adaptation (2026-10-21, source baseline `b35ce457`):
new adaptive sessions select `playout_input_stable_adaptation=6`. This supersedes
revision 5's modeled-readiness percentile as the source of buffer demand.
The actual eligible preventable-input-hitch coverage over the user's selected
history, shared across source-rate layers, governs adaptation. The displayed
120-second percentage uses these same outcomes; CPU interval severity remains
a separate Client timing metric.

When actual misses exceed the configured allowance, the buffer can grow only
toward protection directly authorized by fresh qualifying hitches. A newly
proven requirement cannot be trimmed away by an older miss percentile while
the actual goal is failing. Harmless late readiness cannot enlarge the authorized
destination. A reduced safety cap still drains excess protection even during a
deficit. When coverage
exceeds the goal, release is continuous at the configured fall rate multiplied
by the unused fraction of the hitch allowance: 99.70% coverage against 99.50%
uses 40% of that rate. Equality holds the buffer. An allowed isolated hitch does
not authorize growth or a clean-window reset. Invalid observations supply no
clean outcomes and stop speculative release; fully expired evidence retains the
existing unknown-history expiry behavior. Upward saved-layer restoration is
also blocked while actual session coverage meets the goal. Caps, physical
readiness, queue safety, source cadence and presentation recovery remain separate.

The existing rise/fall bounds and proportional near-minimum taper remain.
Historical revisions 0--5 replay unchanged; the existing revision-based
calibration identity separates the new policy. This correction does not change
the decode-completion measurement or establish physical display improvement.

PyroWave producer-clock queue-residence correction (2026-10-21, source baseline
`04ad823f`): new bounded-catch-up sessions select
`normalized_source_intrinsic_decode_gpu=1`. The normalized producer clock uses
GPU service samples only when completion collection started within 1 ms of
immutable decoder output. A later collection can include intentional pacing
queue residence even if its final API wait exceeded 200 us. The last 64 prompt
samples supply a median; without one, immutable output anchors the clock.
The worker still establishes actual GPU readiness before rendering. Input
adaptation, source cadence, queue limits and physical presentation floors are
unchanged. Captures omitting the new field select zero and replay historically;
the calibration identity includes the new clock selection.

The newest completed matching 4:4:4 10-bit capture is
`moonlight-pacing-20261021-195637-128136-connection-4.vrrtrace`, SHA-256
`53629cfae8eeaf15cffdc90dc0330faa1de09b0e004a441464661caf9d8b8cc6`.
The later final connection uses 4:2:0 and is retained as a separate baseline.
The 4:4:4 capture passes strict exact replay and sequence-integrity checks.
A deterministic delayed-collection regression fails before the correction:
producer-clock skew is 15.2 ms; afterward it is 0.2 ms. A prompt genuinely long
GPU decode retains its full service duration. On the identical fixed-admission
capture, median decoder-output-to-submission falls 25.016 -> 22.034 ms and p95
28.369 -> 24.431 ms; presented jerk over 2 ms remains 92 per mille, with p99
8.221 -> 8.200 ms. Neither scenario saturates. Fixed-admission replay retains
captured discarded frames and cannot prove that a live candidate avoids them.
GPU/compositor service and physical display improvement require a fresh live
comparison. Evidence and the preserved exact baselines are under
`build/444-regression/`.

Display-ceiling policy 2 (2026-10-20, `playout_ceiling_phase_replacement=2`):
policy 1 (below) shed the phase every late frame forced with a replacement, so
at the ceiling each late frame cost a visible skip. The gate now models
Moonlight's fixed-refresh pacer (`Pacer::handleVsync`: drop target 1, or 3
while any of the last 500 ms of queue samples held at most one frame; a
1000/Hz - 3 ms wait; a three-frame queue; shown on the next vsync), scored as
the mean over 16 vsync phases on identical input. Fixed refresh carries about a
queued refresh of latency (13.5-16.3 ms in the model against VRR's 2.5-11.7 ms)
and therefore rides out a late frame under a refresh with no repeat at all.
Policy 1 showed 12 skips a minute at one 4 ms late frame per 5 s where fixed
refresh showed none.

Policy 2 keeps on-screen phase that the frame's own late readiness forced, up
to one refresh, as protection for the next late frame, the fixed-refresh queue
equivalent. The 32-frame rule now sheds only removable phase, the part beyond
each frame's own readiness, above the larger of the smoothing budget and the
kept phase, plus T. Late input that persists (a latency step) is therefore not
removable and costs no periodic replacement. Beyond a refresh:
- A step past a refresh requests replacement at once. The step can be in
  on-screen phase (a late frame or a burst) or in removable phase (late input
  turning on time again). It needs a previous shown frame, and a new shed starts
  at most once per 32-frame window. A lossy capture span stepped every few frames;
  shedding each step cost 50 replacements where policy 1 took 14.
- Shedding continues through a burst while each replacement sheds, and would
  shed, at least half a refresh, so a latency step never discards frame after
  frame.
- A frame itself more than a refresh late accepts only a successor ready within
  a quarter refresh: an on-time successor would already be decoded. Other
  requests wait the usual two periods (`VrrTimingDecision::replacementWaitUs`).
- Only the stepping frame asks. A request on every frame compounded the
  worker's waits into the phase it was shedding.
- The worker's wait for the frame's own GPU decode (`decodeSyncWaitUs`) is
  never shed at once; decode backlog stays with the 32-frame rule.

In the gate, latched VRR has no more visible events than fixed refresh for every
synthetic input and profile, never freezes longer than fixed refresh's worst hold plus a
refresh, and has lower latency:

| Input | VRR events (60 s) | Fixed refresh events | VRR latency | Fixed refresh latency |
| --- | --- | --- | --- | --- |
| 4 ms late frame every 5 s | 0 | 0 | 5.7 ms | 13.5-13.7 ms |
| 8 ms late frame every 5 s | 0 | 7.5-7.9 | 9.7 ms | 14.3-14.5 ms |
| 10 ms late frame every 5 s | 0-12 (one merged skip each) | 13.4-13.6 | 2.5-11.7 ms | 14.5-14.7 ms |
| 2% of frames 2 ms late | 0 | 0 | 3.9 ms | 13.5-13.7 ms |
| 25 ms stall every 10 s | 6 | 12.4 | 2.5-3.5 ms | 14.4-14.7 ms |
| 20 ms latency step for 2 s | 6 | 6.0-6.2 | 4.7-5.6 ms | 15.6-15.7 ms |
| The same step opening with a bunched pair | 6 | 8.4-8.6 | 4.7-5.6 ms | 16.2-16.3 ms |

Trace-driven model of the 2026-10-20 captures (exploratory: the gate's worker
model on each capture's recorded arrival timeline, latched, without present
feedback; a frame that needed no native decode wait counts as ready at decoder
output; replay cannot shed frames):

| Capture | Replacements (policy 1 → 2) | Visible events at 120 FPS intervals | Fixed refresh events | Mean latency, policy 2 vs fixed refresh |
| --- | --- | --- | --- | --- |
| 03:58 (`B1A42391...`) | 32 → 2 | 140 → 83 | 325 | 11.1 ms vs 15.0 ms |
| 04:20 (lossy span) | 14 → 7 | 234 → 222 | 661 | 10.6 ms vs 14.8 ms |
| 05:02 (`304227AD...`) | 0 and 0 | 74 and 74 | 126 | 10.8 ms vs 13.6 ms |

At 05:02 the source ran at 120 FPS for only 7% of intervals. Fixed refresh
receives frames in decode order, so a late frame holds back its successors.
Two spans of these captures are gate fixtures (`tests/vrr/capturespans.h`,
`versus-fixed-refresh-captures`):
- 03:58 frames 25000-26999: policy 2 shows 6 visible events and 0 replacements;
  policy 1 showed 31 and 10, fixed refresh 6.1. On real input, policy 1 was
  worse than fixed sync.
- The lossy 04:20 span (frames 21000-22599, 61-364 lost packets per frame,
  20-46 ms late): policy 2 shows 54 events and 8 replacements, policy 1 56 and
  10. Without the 32-frame rate limit there were 49 replacements and 93 events.
  Fixed refresh shows 28 there, at 5.5 ms more latency. Its deeper queue hides
  chaos that Balanced's adaptive buffer does not rise fast enough to absorb, so
  the gate lists that span as an open known regression of the buffer, not of
  ceiling replacement.

Cost: after a late frame, latency stays up to one refresh higher until a long
source interval repays the kept phase (03:58: 10.6 → 11.1 ms mean). A presenter
that cannot latch still replaces about one frame in 48 at exactly its maximum
refresh, about 150 a minute where fixed refresh's FIFO vsync shows 0-14. Policy
2 trims policy 1's count by a fifth; a cure needs a latched or vsync-locked
present at the ceiling. The gate lists it as an open known regression.
Display-ceiling phase replacement (2026-10-20, `playout_ceiling_phase_replacement=1`):
at exactly the display ceiling no presenter can repay inherited phase by
showing frames faster. The 2026-10-20 03:58 capture (SHA-256 `B1A42391...132B`,
120 FPS on 120 Hz, DXGI latched, Absorb revision 7, input policy 10) kept
3-7 ms of catch-up carry for eight minutes, about 5 ms at a sustained 120 FPS:
input-buffer release and late frames became flip-queue ownership phase. The
ready-successor replacement existed only for native scheduled presentation,
and its trigger read the planned retiming, which smoothing revisions 8/9 cap to
their budget while a synchronized flip still waits a refresh after the last.
In the controller model the published build therefore showed retiming at its
budget yet 9.96-11.3 ms latency against 5.6 ms from a floor start.

New sessions record parameter 1: the replacement applies to every presentation
path and smoothing mode. Its trigger takes the larger of the planned retiming
and the on-screen retiming (a latched frame's flip estimate relative to its
smoothed slot, less 1 ms of unabsorbable source displacement), against the
smoothing budget (0, T or 2T) plus T over 32 qualified frames. A replacement
resets the T-per-second recovery slew and takes the pending input release with
it, so one dropped frame sheds a starting delay instead of a dozen. Modeled
Balanced latency after a cold start at 120/120 now equals a floor start
(2.67 ms Reduce judder, 3.61 ms Absorb) with one replacement for the release
and one per injected late frame. Away from the ceiling nothing is replaced.
Recorded captures replay unchanged; replay cannot shed frames, so these are
controller-model results from `tst_vrrbufferregressions` display-ceiling.

Known discrepancy: a presenter that cannot latch must stay a guard above its
previous present, so at exactly its maximum refresh it cannot show every frame.
The interval buffer then reports service overload, which blocks all input
evidence; the starting delay never releases (+6.9 ms Balanced) and phase
sawtooths between replacements. The gate lists it as an open known regression.

Rate-drop layer drain (2026-10-19, input policy 11): new adaptive sessions
select input-hitch policy 11. It is policy 10 plus policy 9's drain of a
smaller layer by at most T per fresh source frame, instead of the ordinary
release law. Fresh-layer reacquisition applies to policies 10 and 11 alike;
policy 9's T-capped rise/fall, zero recovery credit, release probes and
rate-drop settling stay policy-9-only. Under policy 10 an abrupt rate drop that
moves the source mapping later kept the old protection on top of it: with 116
FPS where one frame in ten arrives early, then 30 FPS without early frames,
decode-to-present averaged 4.2 ms (Balanced) and 5.8 ms (Smooth) over the next
15 s against 1.2 ms settled, clearing in 25--30 s (the f96b0524 symptom).
Policy 11 averages 1.19 and 1.14 ms on the same input; a layer change still
removes at most T in one frame. Layer selection is unchanged: confirmed layers
at least 30 FPS apart, so a 116 -> 90 -> 116 FPS ramp, random 90--116 FPS
dwell and the long tails that once switched a 91 FPS layer cause no switch,
and the faster drain cannot fire inside one operating band. A three-frame
stall excursion at 116 FPS costs no protection.

Policy 11 also keeps the steady reassembly-to-decode handoff in input
readiness. The newest capture (2026-10-19 23:11, SHA-256 `7CC78F72...291C`,
22528 frames at 120 FPS) spends 75--207 us there on every frame (p10--p99,
median 97 us; 0.16% over 250 us). Earlier policies subtracted all of it as
client backlog, but the source mapping anchors on decode completion and
includes it, so every requirement read about 0.1 ms low and protection fell
short by the same amount. Only queue time above the median of the last 64
frames now counts as backlog; decoder-backlog spikes still buy no buffer.

Recorded policy 10 replays unchanged; replay configs accept 0--11. Calibration
identity includes the policy number, so the first policy-11 session for a key
starts from the cold delay. These are controller-model results from
`tst_vrrbufferregressions` plus one capture's decoder-queue distribution, not a
live A/B.

Immediate bounded smoothing (2026-10-19, source baseline `3ae5f3b0`):
new ordinary Reduce judder sessions select cadence revision 9. It retains
revision 4's source-gap dead zone and total [-T,+T] phase bound, but applies
that correction on the affected frame instead of slewing it at T per second.
New Absorb sessions select revision 8: the qualified local-rate predictor
spends a fixed 2T reserve immediately and fully compensates source displacement
inside that reserve, rather than blending part of every gap back into output.
Occupancy remains [0,2T]; it cannot acquire a learned reserve or borrow the
input buffer. Deliberate recovery shares the unused positive phase budget.
Recorded revisions 0--7 retain their exact historical behavior. Calibration
identity already includes the cadence revision. Input policy 10, stable input
adaptation 5 and receive-clock revision 4 remain unchanged.

These intent bounds are not a hard bound on the graph's Retiming budget.
That graph publishes `plannedExtraDelayUs()`, including native ownership phase.
The earlier isolated test disabled input adaptation/clock adjustment and
bounded only smoothing plus recovery. Its replay also contained total extra
values above the reserve. It did not establish absence of a full-rate plateau.
The revised tests check the actual graph metric with the live adaptive policy.

At the exact display ceiling, native scheduled output cannot repay inherited
phase by showing every frame faster. Shrinking input protection can otherwise
leave the old delay permanently in ownership phase. Revisions 8/9 request a
replacement only on qualified, continuous source cadence within 0.1% of the
physical ceiling, and only when the minimum retiming over 32 frames exceeds
the positive phase budget plus T. A high point in normal short/long game
intervals cannot trigger replacement. With actual refresh headroom, ordinary
recovery remains active without this replacement gate.

The worker reserves a successor before discarding the current frame. It may
wait at most two source periods (capped at 20 ms) for admission/prepared
readiness, interruptibly on stop or window transitions. Async decode proof uses
the backend's existing bounded completion check outside the queue lock. Its
service time and observation timestamp are consumed once on the next loop.
Unverified completion keeps the current frame and processes the reserved
successor next; absent future frames never justify discarding the sole image.
Readiness and physical ownership floors remain mandatory. The resulting
occasional replacement is an explicit latency/cadence tradeoff at the ceiling,
not a guarantee that all source images can be shown after a late presentation.

`sourcegapsmoothing` now compares recorded/corrected policies on 0.5--1 ms
variation on every source interval at 90, 116 and 120 FPS on a 120 Hz display,
with both ordinary Reduce judder and Absorb, on worker and scheduled paths.
Adaptive input buffering, clock adjustment, nonzero submission overhead and
actual graph-budget assertions remain enabled. Recorded immediate output
reproduces >=1.7 ms interval-change p99; recorded full-rate Reduce judder also
reproduces >6 ms processing latency. A separate 100-second seeded-buffer test
reproduces a sustained 4.5--6.9 ms native phase before replacement recovery.
Worker fixtures distinguish verified from unavailable GPU readiness and check
progress, ownership and strict replay separately. These are software/model
contracts; live display smoothness must be assessed separately. Artifacts are
under `build/absorb-retiming-contract-20261019/`.

Buffer coverage extension (2026-10-19): `testBufferDisturbanceContracts` runs
336 live-policy combinations of seven disturbances, native/worker presentation,
ordinary/Absorb smoothing, T=250/500/1000 us, 116/120 FPS and 0/6000 us seeds.
Each runs 12000 source frames with adaptation and clock mapping enabled and
crosses RTP rollover. Disturbances cover deterministic nonperiodic 0--1 ms
source displacement, three-frame decode-output and native-acceptance delays,
60 FPS transitions, a source pause, invalid timestamps/rebase, and clock drift.
Assertions include matching-frame readiness, native spacing from accepted
targets, shared timestamp intent, actual graph/combined budgets, settled
processing p99, peak startup/fault latency, duration of excess delay, and bounded
replacement counts. Missing timestamps explicitly exercise fallback trajectory
recovery; it does not have the timestamp reserve's component limit. Its real
latency and eventual recovery remain checked. A 1 ms unabsorbed source
displacement at the physical ceiling may create ownership phase independently
of a smaller T; the graph bound accounts for that known source displacement.
Two disabled controls verify that Absorb alone never activates smoothing.
Worker coverage additionally charges nonzero successful and retried decode
waits, preserves observation timestamps and checks exact replay for all four
readiness cases. Evidence is under `build/buffer-coverage-20261019/`.
The retried-wait fixture exposed a replay audit blind spot: a failed wait with
nonzero service time was incorrectly required to prove GPU completion. When
the trace explicitly supplies `decode_readiness_observed=0`, replay now accepts
an unchanged decoder-output boundary only if the wait fits between dequeue and
decision. Missing-state historical captures retain the previous strict rule;
claimed completion, impossible wait durations and invalid ordering remain
rejected. Controller replay and production timing policy are unchanged.

Trace regression gate (2026-10-19): replay's `Distribution` and
`SenderCadenceTracker` now live in `tests/vrr/vrrreplaymetrics.h` and are used
unchanged by replay and `tst_vrrtracemetrics`. The new test checks independent
tail ranks, missing durations, sender fidelity versus output evenness, hitch
thresholds/attribution, source versus delivery-gap denominators, matching
CPU/display graph lanes, and red-marker allowance semantics. The maintained
trace-question/test mapping is in `tests/vrr/README.md`.
`run_trace_contracts.py` requires named critical targets, runs all built VRR
suites and strictly replays four newly generated worker readiness fixtures.
Each run uses a fresh artifact directory and checks process exits, exact state,
nonempty replayed population and sequence/timestamp/row/session integrity.
Windows CI calls the same gate. An assertion-source audit and executing release
assertion sentinel prevent NDEBUG from turning test assertions into no-ops.
This repairs missing assertion guards in receive-clock/cache-identity tests
and the corresponding Linux feedback/scanout fixtures. It does not change
production timing or certify hardware scanout from a software model.

Ultracode coverage hardening (2026-10-19): the integrated input contract target
adds physically timed downstream work with a genuine-input-miss control,
fractional cadence near multiple display ceilings and expiry of earned input
protection through invalid observations/epochs. Metric controls add partial
populations, non-divisible percentile ranks, uncertainty, clock origins/restart,
expiry and delayed image identity. They exposed one scoring defect: unavailable
local decode/submission timestamps could become a predecessor and bridge clean
client-spacing evidence across a gap. The shared tracker now breaks adjacency
for missing local timestamps while retaining valid RTP/source zero. This does
not change the pacing policy. Runner validation rejects malformed/contradictory
result populations and leaves failed runs incomplete; replayed equals scheduled,
delivered bounds scheduled, and presented is bounded by scheduled. Adversarial
runner tests exercise main(), and eight isolated scoring mutations are rejected
by release assertions. The workflow evidence is under
`.workflow/ultracode/trace-contract-hardening-20261019/` and build artifacts under
`build/ultracode-trace-contracts-20261019/`.

Watcher round 2 (2026-10-19): input regressions now check scheduled delay
through earned growth and expiry, in addition to observer statistics. Controller
copies pinning the output to 1000 or 6000 us fail those checks. Histogram tests
constrain exact known bucket endpoints and sparse-page ranks, preventing both
understatement and arbitrary inflation of reported latency. Strict replay also
joins a resync reservation to the next owned image and its proof kind, rather
than accepting any larger frame ID and valid enum. Prepared proof uses the
job's stage-ready time; retained decode waits must prove completion by the
drop. A precompleted image may be rechecked after reservation, so that later
upper bound does not disprove the earlier readiness claim. Stop/suspend may
discard the matching image without stage telemetry. An unresolved reservation
fails lifecycle integrity. The regression gate recomputes valid capture hashes
for wrong-identity, wrong-proof and missing-successor controls and requires
causal lifecycle rejection, alongside four exact valid fixtures. These are
diagnostic/test changes; production pacing policy is unchanged. Evidence is
under `build/ultracode-watcher-round-2/` and the preceding workflow's
`watcher-round-2/` directory.

Rolling required-buffer convergence (2026-10-17, revision 5): new adaptive
sessions select `playout_input_stable_adaptation=5`. This replaces revision 4's
hard historical-score hold and revision 3's speculative plateau reductions.
The active cadence layer retains each eligible frame's budget-adjusted required
protection duration as observed, expiring by the user's configured history (10--300 s;
120 s is only the Balanced preset). Copied seeds, invalid observations and
unmatched epoch boundaries do not supply modeled clean intervals.

For a constant candidate buffer, the input-only model propagates outstanding
lateness using each interval's bounded recovery room. A new input can cause a
stretch above tolerance only when it exceeds the preceding carried requirement
by more than tolerance. Such an interval contributes its required protection;
other matched intervals contribute zero. Proven same-frame actual input hitches
override a modeled recovery assumption, so estimated carry cannot hide a measured
protection requirement. The selected rolling percentile is the
smallest buffer covering the user's configured share of modeled intervals.
Steady late input therefore does not manufacture recurring hitch pressure.
Old requirements are not reprojected into today's clock mapping: the estimate
answers what protection the retained frames needed when they were delivered.
Candidate scoring recomputes outcomes at the candidate size, rather than counting
past misses recorded at different applied buffers. Actual outcome reporting stays
separate. A fresh actual eligible preventable hitch still authorizes upward
protection; raw harmless input alone cannot buy additional standing latency.

The full session history calibrates the modeled allowance against actual goal
error. Below goal, modeled tail allowance becomes user_tail^2/actual_miss_rate;
at or above goal it is exactly the user's allowance. This continuous correction
permits some modeled misses and helps a deficient window recover without a hard
hold. A real full-window deficit also authorizes the measured rolling estimate
for growth; clean input while at goal cannot buy unrelated standing latency.
Actual reporting and the user-selected goal/window are unchanged.

Downward movement converges proportionally toward the estimate, capped by the
existing configured fall rate. Upward movement keeps the established rise rate
so an asymptotic approach cannot prolong proven misses. The release time
constant is history/30, bounded
to 1--10 s (60/120/300 s presets give 2/4/10 s). Fractional movement is preserved
and the release rate tapers near the target. The old below-goal hold and 200-frame wait
are absent. Larger learned layers retain immediate upward restoration; smaller
layers converge normally. Historical revisions 0--4 retain exact replay behavior;
calibration identity includes the revision. This input model does not resimulate
worker admission, GPU contention, compositor timing or physical scanout.

Selected-window release gate (2026-10-17, revision 4): new adaptive sessions
select `playout_input_stable_adaptation=4`. The full session coverage history
(Balanced: 99.5% over 120 seconds), aged on the session clock across source-rate
layers, now gates downward input-buffer movement. A below-goal window
holds applied protection, including lower measured demand and lower-layer
drain. A new qualifying miss also prevents that observation from releasing.
Growth keeps its existing eligibility and measured-demand rules. Meeting the
selected goal resumes revision-3 continuous release at the configured fall rate,
without a 200-frame wait, a replacement fixed hold, or accumulated drain credit.
The 10-second displayed score and one-second local feedback cannot override this
gate. Empty history retains ordinary obsolete-seed expiry; it is not counted
as clean evidence for a probe. Historical revisions 0--3 retain their recorded behavior for exact replay;
calibration identity already includes the revision. Target demand may remain
below the held buffer. This is an input-buffer policy, not a guarantee that the
selected score is achievable or proof of physical display smoothness.

Continuous plateau release (2026-10-17): new adaptive sessions select
`playout_input_stable_adaptation=3`. This supersedes revision 2 below.
Qualified clean feedback can continuously lower a retained raw-input plateau
at the configured fall rate (normally 250 us/s). The 100 us / T/4 target
lookahead no longer starts a one-second / 200-eligible-frame waiting period
after each completed reduction. No replacement ten-second clean hold is added.
The existing eligibility, fresh qualifying-miss feedback, physical recovery
room, per-interval movement allowance, ordinary growth and layer restore rules
remain unchanged. The configured smoothness target and learning window are
unchanged; this is not the proposed ten-second proportional-score controller.
Historical revisions 0--2 retain their recorded behavior. Existing calibration
identity includes the stable-adaptation revision. Live smoothness is unverified.

Bounded plateau recovery (2026-10-17, reconciled source baseline `2689cd7a`, originally `7d1148f9`): new adaptive
sessions select `playout_input_stable_adaptation=2`, retaining input-hitch policy
10 and the configured probability/history. Revision 1 remains explicit for
historical exact replay.

Revision 1 disabled all release probes to avoid oscillation, which also disabled
measured-surplus acceleration and left a raw readiness quantile able to pin
protection even without fresh preventable hitches. The completed
`moonlight-pacing-20261017-171738-793163.vrrtrace` contains an 8,621 us plateau
for 107.199018 s (frames 39926--51181), with the qualifying miss total unchanged
at 372. SHA-256:
`272a4d9927c706d860b912d6123a8f995a1b410494d8995756d9e2cc74b8f466`.
The untouched capture passes both exact-baseline and sequence-integrity gates.
This is separate from the initial presentation-starvation episode in that file.

Revision 2 reuses finite feedback-qualified release trials: at most
min(100 us, T/4), followed by at least one second and enough new eligible outcomes
at the reduced buffer to support one expected tail observation (200 at 99.5%).
A fresh qualifying miss cancels a trial and preserves the existing growth gate.
Invalid observations and copied layer seeds cannot supply clean feedback.
Both measured falling demand and trials retain the configured normal fall rate
(250 us/s in this capture); the existing per-interval physical-room limit still
applies. Ordinary release is unchanged. Trial reductions do not redefine the selected target, erase old misses,
reset the rolling history, or continuously chase a lower buffer. Larger learned
layers may still restore immediately; smaller layers drain normally and cannot
save inherited excess as their own learned floor. Cache identity includes the
stable-adaptation revision. Input buffer, extra delay and real decoder-to-CPU
submission latency remain separate quantities.

The compact numeric trace fixture retains all decisions from stream start.
`tst_inputtracechunks` checks every revision-1 buffer state before scoring local
revision-2 progress. It consumes recorded input and recorded submission feedback;
it does not model alternate worker admission or physical display. Rolling 1/2/5/10
second diagnostics complement, and never replace, the selected 120-second
learning window. Startup presentation starvation is reported separately and is
not repaired by this buffer adaptation change.

## Receive-clock correction (2026-10-17)

New sessions select input clock-relative history revision4. Revisions0--3 keep
historical replay semantics. The drift observer fits packet-receive/RTP offsets,
using100ms bucket minima over the existing120s clock-fit window, rather than
fitting producer mapping that includes decoder service. Buffer learning history,
coverage target/tolerance, rate layers and actuator rise/fall laws are unchanged.
A material change in actual source cadence starts a new drift-fit window without
resetting buffer evidence or accumulated clock coordinates; it retains the last
qualified oscillator slope while reacquiring. Clock qualification is independent
of cached FPS layer selection. Invalid packets do not become evidence and an
unrefreshed slope expires after120s. Calibration identity includes clock revision.
Cache identity resolves the same effective adaptive session options as the worker
before constructing policy tags, so new clock sessions cannot share an older
clock policy's startup-delay namespace. The configured goal/window remain learning
objectives; native synthetic tests can still contain below-target windows. Native
correctness evidence and target attainment must be reported separately.

Buffer quality observability (2026-10-17, accepted base `75e97d26`):
the CPU optimization and eligible-age/fresh-reacquisition correction are now
separate local commits. The staged native A/B binaries remain unchanged.
Decision-applied B (`playout_delay_us`) is distinct from the historical
post-feedback controller snapshot. Opt-in `MOONLIGHT_VRR_BUFFER_AUDIT=1` adds
typed before/after scalar snapshots, stable cached-layer identity, eligible
evidence age, qualification/authorization reasons and normal slew budgets.
The existing bounded asynchronous trace handoff owns formatting and accounting.
Frozen-input `vrrreplay` and production-worker virtual-clock `vrrworkerreplay`
answer different questions: only the latter recalculates admission/drops, and
neither proves GPU/compositor/optical or network behavior. See
[Buffer quality contracts](tests/vrr/BUFFER-QUALITY.md) for CLI, provenance,
metrics, missing-evidence handling, predeclared workloads and overhead limits.

Historical offline exact-score experiment: interval-quality integer totals update on ring
mutation/expiry. Weighted sums retain the original physical-index floating-point
addition order; sparse zero-only buckets are skipped. Reporting/control policy,
source timing, target/window and scoring are unchanged. This experiment is not
installed and is separate from the frozen CPU-only application.

Isolated exact segmented input-window experiment (2026-10-17, based on `7d1148f9`):
`InputPercentileBuffer` retains chronological samples for expiry and outcomes,
with small sorted blocks indexing raw requirements and proven misses. Integer
nearest ranks, duplicate multiplicity, selected target/history, seeded layers,
source coordinates and adaptation rules remain unchanged. Valid adjacent-pair
coverage is maintained on insertion and expiry; large expiry advances compact
indexes once. Splits/merges bound ordinary shifts to a small block rather than
moving the entire retained history. Block allocation and retention are CPU and
memory tradeoffs, not a statistical or timing-policy change. This experiment
is isolated and has no physical display validation.

Stable input adaptation (2026-10-17, based on `181415ec`): new adaptive
sessions retain input-hitch scoring policy 10 and additionally select
`playout_input_stable_adaptation=1`. This supersedes the release behavior below.

The selected long-window percentile and actual-miss protection floor size the
learned target. Release follows genuine falling demand at the established
250 us/s rate. Repeated speculative 100 us target reductions and their accelerated
roughly 5 ms/s release are disabled. Growth still requires an eligible,
same-frame readiness-attributed preventable hitch below the selected goal, is
limited to its measured requirement, and uses the normal configured rise rate.
Unprotectable requirements, invalid mappings, packet loss, overload, harmless
readiness overruns and post-input timing errors keep their existing exclusion
rules. The score's definition, selected target/history, and recent reporting
window do not change. A substantially larger learned layer can restore protection
at once; smaller layers retain applied protection and use the same normal drain.

Timing recovery remains bounded by the current PhaseTrajectory, physical floors
and intentional extra-delay budget. The historical uncapped timing-recovery
ablation is not selected: on the latest completed capture it reduces
modeled jerk while accumulating unacceptable modeled latency. Input adaptation
is therefore restored independently of scoring and timing recovery. Old captures
without the new parameter select zero and preserve their historical release law.
Cache identity includes the effective stable-adaptation revision.

Matched final-policy replay uses the newest completed capture selected for this
iteration, `moonlight-pacing-20261017-005745-33392.vrrtrace` (SHA-256
`ea74dd760492a91a666f3f62dba248c9e1559418378b9dbd5d085df5fd9606ef`).
The untouched capture passes exact baseline and sequence-integrity checks.
Stable adaptation reduces qualifying input misses from 400 to 201. On matching
frames outside the first five seconds and one second around rate/phase changes,
within continuous matched spans, total buffer movement falls 52.471 to 30.877 ms,
with reversals of at least 400 us falling 16 to 13. Mean decode-to-submission rises 14.868 to 15.365 ms,
and p99 32.247 to 32.415 ms. Modeled presented-jerk pairs over 2 ms fall
20.7% to 19.5%; sender-spacing residual p99 is reported separately in the replay
artifacts. All five nominal/decision/preparation/submission/scheduler scenarios
pass explicit buffer, mean/p99 latency and modeled interval-safety bounds without
worker saturation. Fault bounds are relative to the unchanged policy under the
same injection and are not normal operating latency. These are software/model
results, not physical display or live integration proof. Artifacts are under
`build/stable-buffer-recovery-20261017/`.

Layer-drain correction (2026-10-17, based on `1a5bef09`): new adaptive
sessions select input-hitch policy 10. This supersedes policy 9 below.

Policy 9 mistakenly coupled the layer rework to slower ordinary adaptation.
Policy 10 restores the established input rise rate (configured 250--1000 us/s)
and healthy continuous release: at most 0.5% of the source interval, T/2 and
available VRR headroom per interval. Fresh eligible coverage, capacity and
miss attribution still qualify adaptation. There is no 200-frame wait between
100 us release probes. Genuine lower measured demand can also use the ordinary
fall rate when accelerated release is not qualified. Fresh qualifying misses
still interrupt healthy release and authorize measured growth; old misses alone
cannot sustain further growth.

Confirmed layers retain the 30 FPS separation, saved evidence and continuous
session coverage. A substantially larger learned layer may restore protection
immediately. Selecting a smaller layer retains the currently applied buffer,
reports the learned destination at once, and drains with the normal release
code. There is no T-per-frame downward shortcut or repeated post-drop snap.
Interrupted transitions store the selected layer's own learned destination,
not protection inherited from the previous layer. Intentional smoothing and
recovery retain their movement budget, but input adaptation no longer spends
that budget or accumulates debt that freezes extra-delay recovery. The fixed
2T Absorb pad and physical readiness/ownership floors remain in force.

Superseded by policy 11 (2026-10-19, see the note at the top): because a
smaller layer drains at the normal release law, an abrupt rate drop that also
moves the source mapping later keeps the old protection on top of the moved
mapping, reproducing the f96b0524 symptom for 25--30 s. Revisions 6/8 with the
rate-drop snap, revision 9's T-per-frame drain and policy 11 clear it within
about 3 s on the same input. Recorded policy 10 retains the release-law drain
for exact replay.

The normal overlay and timing graph report recent eligible coverage over 10 s.
The normal overlay also shows the full selected history (Balanced: 120 s),
which remains the controller's learning history. Both reporting windows count
actual matched eligible outcomes, survive rate changes and expire individually;
invalid input never counts as clean, and an empty window reports N/A. This
changes reporting responsiveness without redefining the selected target or
resetting history at layer changes. Historical policies 0--10 remain explicit
for replay, and cache identity includes the policy number.

Nightly VRR integration (2026-10-17, master baseline `f0507ea5`):
that baseline selected input-hitch policy 9 and Absorb smoothing revision 7.
Current sessions select input policy 11 and smoothing revisions 8/9 as described
above; the shared actuator details in this historical section apply to revision 7.

Policy 9 caps ordinary input-buffer growth and release at the selected interval
tolerance T per second. Intentional smoothing and recovery share the remaining
movement allowance after the input buffer has moved; they cannot each add another
T/s. Fractional credit preserves small rates at high FPS. Physical readiness and
scanout ownership floors remain mandatory, are reported, and are not invented
adaptive-buffer demand. Actual input readiness never moves its own scoring deadline.
The source-clock estimator is a separate clock transform, not another buffer;
policy 9 no longer remaps applied protection during the post-rate-rise window.
A shrinking FPS-dependent maximum lowers demand without instantly truncating
already applied protection.

A layer switch requires at least 30 Hz separation in rounded source FPS, in
addition to policy 8's symmetric 1.5x period departure and consistent RTP-run
qualification. Both detection and saved-layer lookup apply this gate. Three
consistent intervals can still qualify a real 90 -> 30 or 30 -> 90 change;
40 -> 20 cannot create another layer. Ordinary fit movement does not switch.
A selected layer restores its saved protection immediately only for an increase
larger than T. Smaller differences slew normally. Decreases larger than T retain
the old applied buffer and drain by at most T per fresh source frame, sharing
that allowance with extra delay. Repeated decisions cannot spend it again.
The same T/frame bound follows falling demand while the new source mapping
settles (at least one second, at most ten, ending once the mapping settles).
It never follows that moving target with an instantaneous downward assignment.
An interrupted drain stores the selected layer's learned target, not inherited
excess from the previous rate. Session coverage reporting remains continuous.

The previous 100 us release probe was renewed every frame and could drain near
5 ms/s as soon as historical coverage crossed its goal. Policy 9 separates
measured surplus from a speculative trial. A trial releases at most min(100 us,
T/4), then waits at least one second and enough new eligible outcomes to support
one expected tail sample (200 frames at 99.5%) before trying again. A fresh miss
cancels it. Old historical misses do not prohibit fresh bounded release forever.
Crossing the long-window percentage cannot revoke a growth request while its
qualifying miss is still in the fresh one-second feedback window. Old misses
alone cannot sustain further growth. VRR credit cannot abruptly remove a whole
standing input quantile; it funds bounded trials and genuine layer transitions.

Absorb's reserve is fixed at 2T, independent of FPS. Revision 7 retains the local
cadence predictor but removes the separately learned reserve (previously up to
5 ms). Timing can spend only the fixed pad, so smoothing adds between zero and
2T and cannot borrow the input buffer or acquire another positive reserve.
Planned timing corrections and recovery pass through the shared slew
budget as well. The reserve itself starts at 2T; source phase adjustments can
spend that reserve, so instantaneous planned extra delay is a separate quantity.
Capping phase corrections as well as adaptation limits how quickly this candidate
can absorb source jitter; this is not a claim of improved displayed smoothness.
Recorded input policies 0--8 and smoothing revisions 0--6 remain selectable.
Cache identity includes the new policy/revision, preventing old learned delays
from being imported as this build's starting protection.

The completed capture selected for this investigation was
`moonlight-pacing-20261016-230433-563754-connection-2.vrrtrace`, SHA-256
`cf4fa07454f2d732f8973c7b3e079da0d6c43cd0ec73445280f68844dcf50982`.
It has 5,847 rows and a clean footer with zero writer drops. It was decoded for
inspection only. Replay simulations and exact-baseline verification remain
deferred at the user's request; deterministic checks do not establish optical
fluidity or physical scanout behavior.

Previous rate-layer test build (2026-10-16, based on `4a4ed116`):
recorded input-hitch policy 8. Policy 7 falsely classified short
source tails against a stale startup/layer anchor as a rate transition. In the
completed `moonlight-pacing-20261016-223958-554910.vrrtrace` (SHA-256
`50bda0f03e84572348c5c425cfa3055cb2285e0e8cf3a575188133ca2dc763ba`),
frames 567--569 have RTP intervals 12.645, 13.311 and 13.600 ms while the prior
fit is 11.095 ms. The buffer steps from 1.526 to 1.000 ms and active coverage
from 410 observations/3 misses to zero. This is a controller-selection defect;
these source intervals alone do not establish a host stall or optical stutter.
The capture has a clean footer and no dropped trace rows. It was decoded for
observation only; replay simulations and exact-baseline validation are deferred.

Policy 8 separates layer selection from the continuously moving source fit.
A fast transition requires three adjacent, consistent RTP intervals departing
by at least a 1.5x period ratio from both the layer anchor and the operating fit
at the start of qualification. The run's full spread must fit within
max(500 us, 10% of its mean). Source intervals are bounded by the negotiated
rate before qualification. Missing identity, an isolated interval, and ordinary
variation cannot bypass this gate through the long fit. A gradual large change
can also qualify after at least 12 tightly clustered intervals spanning 200 ms,
with spread at most max(500 us, 5%) and the fit within 10% of their mean.
A confirmed return resumes the nearest matching saved layer. Lookup uses the
same 1.5x boundary; the active layer wins until a transition qualifies.

Each layer owns its learned protection and adaptation evidence. Eight own
eligible observations qualify retention; a previously qualified layer survives
brief return visits. Inactive history still expires. Physical queued frames
remain chronological; layers store control state, not video. Policy 8 preserves
the observed timing percentage in a separate session-wide trailing history of
actual eligible outcomes. It survives layer changes and source rebases, expires
at the profile's selected window, and resets at a new session. Invalid samples
never count as clean. Internally, growth/release still use each layer's own
coverage. In policy-8 telemetry and trace rows, `input_percentile_recent_*`
report this continuous history; `samples` and release feedback remain local to
the active layer. Recorded policies 0--7 preserve their prior behavior.

Inside a layer, growth follows the selected rise rate and healthy release uses
policy 7's bounded probes: at most min(100 us,T/4) below a percentile plateau,
with fresh qualified input, at least eight own observations and 500 ms of valid
pair coverage, no qualifying miss in the last second, and actual coverage at
or above goal. Accelerated release shares a per-interval budget bounded by
0.5% of the intended interval, T/2 and available VRR credit. Policy 8 removes
the repeated post-drop `followTarget()` snap window; only a confirmed layer
transition may restore or credit protection immediately. Rate-rise mapping
coordinate correction remains distinct from buffer adaptation, and the existing
PhaseTrajectory still bounds actual cadence recovery. Cache identity includes
`input-hitch-misses=8`. Deterministic checks and the local application build do
not establish visible fluidity; live testing is still required.

Absorb microstutter correction (2026-10-16, after bcb06f04): new Absorb
sessions select source cadence smoothing revision 6. The previous revision-2
PLL repeatedly reseeded its short-period predictor from a longer source fit
that still contained earlier slower intervals. After the source had already
settled to uniform faster output, it could recreate up to 8 ms positive
retiming, reset it, and repeat. Four consecutive valid RTP gaps all within the
selected tolerance T of their mean now qualify that local source cadence even
when the longer fit has not caught up. When its predictor differs from this
qualified mean by more than T, it reseeds to that mean rather than repeatedly
reverting to the old long fit. Nonuniform quantized/alternating source output
keeps the phase-locked smoothing path. This changes the short-term smoothing
predictor, not the source timestamps, main cadence fit, input-miss classification
or independent selected protection bounds. Ordinary Reduce judder remains
revision 4. Cadence-reset retiming releases by at most min(1 ms,T/2) per frame,
250 us at default T=500 us. Reserve cap/percentile/attack/release stay unchanged:
5 ms, p99.5 over 128 placed frames, 250 us per-frame attack, 500 us/s release.
Recorded revisions 0--5 retain their explicit behavior for exact replay.


Matched final replay for `moonlight-pacing-20261016-202136-501609.vrrtrace`
(SHA-256 `9211611e889885f748f0e7996a4fe456ea9313215c754f3c45183318f4249e03`)
compares input policy 6 with old smoothing 2 versus smoothing 6/reset 250 us.
Mean decode-to-submission falls 6.508 to 6.309 ms and p99 15.151 to 14.154 ms.
Requested Extra p99 falls 8.000 to 7.547 ms, standard deviation 1.738 to
1.583 ms; display-jerk p99 falls 3.085 to 3.015 ms with >2 ms pairs unchanged
at 18 per mille. Actual eligible misses change from 50 to 52 out of 15,904;
the final window changes 11 to 12 out of 3,190, remaining above the selected
99.50% goal (99.655% to 99.624%). The uniform 80-to-116 FPS fixture reduces
mean Extra 1.187 to 0.171 ms and mean jerk 67 to 10 us across all presets.
Two independent older Absorb recordings improve mean latency about 0.13 ms
with a one-per-mille increase in >2 ms display-jerk pairs; this is not universal
smoothness dominance. The final ordinary steady-rate replay reproduces exactly.
Untouched old-policy replay passes exactness and recorded-sequence integrity;
ten nominal/decision/preparation/submission/scheduler stress scenarios pass
explicit buffer, decode-to-submission p99 and modeled interval-safety bounds,
without worker saturation. These are replay/model results, not optical proof.
Final artifacts are under `build/extra-delay-stability-fix/`.

Extra-delay phase accounting correction (2026-10-16, after `bcb06f04`):
`Extra delay` is the nonnegative net phase requested by smoothing, display
ownership and retained recovery, beyond the input buffer. The previous worker
added cadence smoothing to the recovery wait remaining after the current
frame's readiness clamp. This mixed phase with marginal wait: in the matched
Absorb capture, frames 14382--14384 reported 3.739, 8.205 and 4.317 ms while
submission intervals stayed near 8.4 ms and retained phase drained gradually.

The trajectory now exposes its requested target before the current readiness
bound, independently of its unchanged actual target. The controller retains
independent display-floor and recovery requests for reporting. Both overlays
consume one derived net phase value. A newly late input itself does not count
as smoothing/recovery protection, and no reported value changes input scoring
or demand. Signed smoothing cancels positive requested phase before clamping
at zero; arithmetic saturates. Replay exposes `planned_extra_delay_us`,
`planned_total_delay_us`, `overlay_extra_delay_us` and a per-frame
`simulated_planned_extra_delay_us` column. Historical `pacer_delay_us` remains
the old marginal-wait diagnostic for comparison. Trace fields stay unchanged.

Rate-transition correction (2026-10-16, after `25a18cfa`): production
selects input-hitch policy 6. A rate-dependent producer baseline change exposed
two separate issues. The live source mapping moved several milliseconds while
the already applied buffer released only at 250 us/s, retaining the correction
again as real delay. The history drift fit also fitted a line through the rate
transition's baseline step, treating phase movement as host/client clock skew.
That shifted retained demand and made a naive buffer-coordinate correction
spend real protection.

For ten seconds after a layer change to a rate at least 25% faster than the last
qualified settled layer, a continuously active layer translates current buffer
and previous requested target by movement of the actual applied source mapping.
The inverse buffer movement preserves the planned source deadline, subject to
selected bounds. It uses the raw applied mapping, not the drift-normalized
history coordinate. A created/restored layer first keeps its own protection;
subsequent movement uses the current live mapping basis. Historical coverage is
not rewritten, and genuine new demand retains the existing actual-miss gate.
Normal steady-state release and physical recovery bounds remain unchanged.

During rate-rise/drop settling, the drift fitter discards its old point window
and estimate. It collects a fresh steady baseline afterward and requires the
existing 15-second span before fitting again. Source-coordinate continuity,
retained input samples, layer history and observed outcomes are preserved.
A deterministic fixture separates an 8 ms rate-dependent baseline step from
actual clock skew, including renewed learning with +/-50 ppm clock drift.

The reported transition is present in the completed
`moonlight-pacing-20261016-200843-497715.vrrtrace` session, SHA-256
`970371d3c310313fd0025dbfd59575c4476516d84c699c8686c119710a876828`.
At frame 2108 a qualifying miss raises protection from 1.00 to 10.26 ms; the
producer mapping subsequently advances about 8.5 ms while policy 2 leaves the
extra protection draining for tens of seconds. The source cadence change itself
is not classified as a hitch by this finding. A newer completed session stays
near 90 FPS and does not contain the transition; it is a separate steady-rate
regression check. Artifacts are under `build/rate-transition-latency-fix/`.

Final `final-v6-*` validation uses a freshly rebuilt replay and unchanged source
hashes. The untouched baseline passes exact reproduction and recorded-sequence
integrity. Across the same 1,509 presented frames at recorded seconds 28--45,
mean buffer falls from 7.743 to 2.573 ms and decode-to-submission from 8.639 to
4.355 ms. Full-session decode-to-submission p99 falls from 9.210 to 6.385 ms;
presented jerk over 2 ms changes from 52 to 51 per mille. The final rolling
coverage remains 62 misses out of 11,178 eligible frames, while cumulative
misses increase from 266 to 277 out of 50,145. This is lower latency with a
small increase in earlier misses, not identical coverage at every point.
The separate earlier judder-coverage recording retains the selected 99.50%
final-window goal (15/3,041 misses, 99.507%, versus 14/3,041 previously), with
37 versus 27 cumulative misses. The steady-90 recording reproduces exactly.
All eleven deterministic test targets and ten nominal/injected-fault replay
scenarios pass, including explicit latency and zero modeled-interval-violation
bounds. Injected-fault bounds do not describe normal operating latency.

Policies 0--5 retain their explicit behavior for recorded replay; 3--5 are
intermediate coordinate-correction designs, not production defaults. Cache keys
include `input-hitch-misses=6`. Replay and deterministic checks do not replace
fresh live/optical validation.

Coverage protection correction (2026-10-16, after `bf4f0992`): policy 2
introduced the retained-miss rule, which policy 6 keeps. Policy 1 could trim a proven playback miss as a
raw-input learning outlier and could lower the request on a following clean
update while coverage remained below goal. Policy 2 retains a separate quantile
of actual qualifying miss requirements in the immutable source clock. It leaves
only floor(eligible frames * permitted miss fraction) of the largest misses
unprotected, then uses the remaining miss quantile as a protection floor while
below goal. Raw late inputs cannot increase this floor. Once a qualifying miss puts the
history below goal, policy 2 can retain the justified target immediately, bounded
by that same frame's measured input requirement minus tolerance. This retains
delay the hitch already incurred; an older larger target or harmless late frame
cannot fund the jump. Ordinary follow-up growth keeps its rise-rate limit, and
capacity and selected-maximum qualification remain required. The floor expires with its
samples and is projected through current clock/budget changes exactly once.
This does not reclassify earlier outcomes as successful. Historical policy 0
(raw inputs) and 1 (original hitch gate) remain exact-replay compatible. Cache
keys include the effective policy version.

Matched replay evidence for the reported judder-on session:
`moonlight-pacing-20261016-194234-486829-connection-3.vrrtrace`, SHA-256
`91b48d9c05eefe51ef2a236ebdf8cbc1eca4f0edf46d31fe44a4a5e3e408121a`.
The untouched policy-1 capture passes exact baseline and sequence-integrity
checks. With the same captured inputs, policy 2 reduces session misses from
142/5541 to 27/5541 (97.44% to 99.51% coverage). Mean input buffer increases
4.34 to 7.34 ms; mean decode-to-submission increases 5.42 to 7.83 ms and p99
11.024 to 13.002 ms. Ten original/candidate nominal and injected-stall scenarios
pass explicit buffer, latency and modeled interval-safety assertions without
worker saturation. These are controller/replay results, not live or optical
confirmation. The final 120-second overlay/rate-layer window improves from 82/3041 misses
(97.30% coverage) to 14/3041 (99.54%), meeting the selected 99.50% goal.

Stats contrast correction (2026-10-16, revised after user review): stats use
high-luminance yellow-green (#D4FF00) text with a thin black glyph outline and
an offset black shadow. Its nominal sRGB contrast against black is 18.1:1 using
the W3C relative-luminance formula (lavender was 12.5:1). This is a text/outline
calculation, not a guarantee of visibility in every game or an HDR measurement.
There is no rectangular stats backing. Both colored text and shadow masks wrap
with the same unoutlined font, so long rows keep their shadow without displaced
line breaks. Pixels between letters remain transparent. Software fixtures check
transparent space and both glyph colors, including forced wrapping, and preview
Deck and 4K layouts over white. The graph panel and status messages retain their
existing styles. The preceding opaque stats backing was rejected and removed.

Extra-delay overlay clarification (2026-10-16, after `bf4f0992`): both the
frametime graph and normal stats show `Extra delay` with a `smoothing/recovery`
explanation and a separate `Planned total` equal to input buffer plus extra.
Extra is the nonnegative net cadence-smoothing, presentation-floor and recovery
correction for the reported frame; it is not an end-to-end latency measurement,
a tolerance allowance, or additional protection used to score input misses.
The worker computes it once for both telemetry consumers. Graph history carries
this value independently of the Buffer line and its dashed buffer-plus-tolerance
threshold. Zero extra remains visible. The stats give extra its own compact row.
This reporting change does not alter scheduling, scoring or the trace schema.

Preventable input-hitch goal (2026-10-16): new adaptive-input sessions set
`playout_input_hitch_misses_only=6` (policy 1 introduced the original hitch gate). A scored miss requires valid causal input
lateness beyond the applied buffer plus tolerance, a measured source-matched
interval stretch beyond tolerance attributed to the same frame's readiness
missing its intended deadline, and qualified spare service capacity. The input
must be protectable within the selected maximum; a larger overrun is excluded
from the preventable goal, never invented as clean playback. Loss, unknown
bases, source/cadence changes and sustained overload retain their exclusions.
The compressed successor of a late frame is not another miss for that hitch.
Late input with steady output spacing stays clean. Post-input scheduler or
present-path errors without an input overrun cannot count as input misses.

The selected frame percentile sizes protection, but growth now also requires
this actual preventable-miss history to be below the profile goal. Raw lateness
alone cannot buy buffer, including during learning or from seeded rate-layer
evidence. Even while older misses leave the history below goal, clean late
inputs cannot enlarge the request beyond demand justified by actual misses. Physical VRR room can reduce demand while the actual goal is met;
it cannot erase an observed hitch or discount its tail when below goal.
Raw network/decode stage misses remain independent diagnostics. The stats row's
coverage now refers to these eligible preventable-hitch outcomes. Its history
still records each frame's actual outcome, not retroactive success after growth.
Old traces default the new parameter to zero and retain the raw-input scoring
and growth policy for exact replay. Calibration keys add the effective `input-hitch-misses` version
so an older raw-input cache cannot silently seed the new outcome policy.

Reduce judder tolerance contract restored (2026-10-16): ordinary Reduce judder
now selects source-gap revision 4 in `vrrTimingParametersForSession()`. Let T be
the resolved preset/custom interval tolerance. Consecutive valid source/RTP gaps
within T of the fitted source period do not change the retained smoothing phase.
For larger errors, correct only the excess beyond T, by at most T per gap; bound
the entire retained phase to [-T,+T]. Clean gaps retain that phase, so they are
not changed to repay earlier smoothing. There is no standing readiness reserve,
period blending, or phase recentering in this policy. A source/cadence reset can
clear the phase, still within the shared bound. With T=500 us the smoothing
phase cannot exceed +/-500 us. Readiness, input buffering, display-spacing and
bounded late recovery remain separate constraints; this is not a T bound on
total client latency or a guarantee that every actual presentation matches its
intended source interval. The separately enabled Absorb microstutter option
uses the local-cadence revision-6 gain/reserve policy described above
(reserve <=5 ms, lag <=8 ms); recorded revision 2 remains replayable.
Recorded revisions 2, 4 and 5 retain their explicit parameters for exact replay;
new session calibration keys include the changed cadence, reserve and lag
parameters so old smoothing profiles cannot silently seed this policy.

The regression investigation below describes the earlier rollback and its
measurements, not the final ordinary Reduce judder policy above.

VRR buffer regression correction (2026-10-16): users reported higher VRR
buffers and coverage dipping to about 98% after daily.183. Replaying 18 Deck
captures (daily.183-era sessions through 2026-10-15; the three Oct 10
dev-build captures cannot replay their reference) under each daily's policy,
from each capture's first-row parameter snapshot, separated four causes:

- daily.184 bounded catch-up held successors up to a whole source period
  behind their slots (25 ms on 30 FPS spans).
- daily.187 bounded the input recovery credit to T, so low-rate spans pinned
  the input buffer near its cap (overlay maximum 9.2 -> 13.9 ms on average,
  25 ms on a Smooth capture).
- daily.184's rate layers seeded a never-seen faster layer with a slower
  layer's lateness, and learning followed the single largest requirement:
  25 ms learned on a 21 FPS loading screen held a Smooth session for minutes.
- daily.188 revision 5 passed jitter within T and placed corrected gaps on
  the tolerance-band edge. Near the panel's maximum rate (about 115 FPS on
  120 Hz) that edge is below the display period, bounded catch-up floored those
  frames, and carry, repaid at carry/32, ratcheted toward a source period:
  4-6 ms more latency for no jerk benefit, absent from the overlay buffer and
  counted as input misses (the 98% coverage). Revision 5 had been validated
  against revision 4, which never shipped, not against revision 2.

Comparisons need display-side metrics. The legacy controller left latched
presents unfloored and let the flip queue hold them, so decode-to-submission,
submission jerk and pacer delay omitted a wait that bounded catch-up schedules
explicitly. vrrreplay now estimates display as max(submission, previous
display + display period) (`replay_display_*` jerk,
`simulation.decode_to_estimated_display_us`) and reports
`queue_inclusive_delay_us`: playout delay + retiming + display-floor push +
deliberate recovery hold + submission-to-display wait, with readiness lateness
excluded, also sampled once per second like the overlay.

Production now uses the revision-2 smoother for Reduce judder and a wider
revision-2 reserve/lag for Absorb microstutter, credits the physical VRR room
scaled by the profile miss allowance, seeds new layers only from layers at
most 1.5x slower, trims one outlier while learning, and caps catch-up carry at
min(4 ms, half a period), the VRR17 bound. A 2 ms cap cost most of bounded
catch-up's smoothness (Low Latency capture 33 -> 56 per mille). Faster or
ramped repayment, credit caps, learning ceilings and revision 5 with a
presentable floor were all on the same or a worse frontier. The initial overlay
change led with the total. The subsequent reporting correction in section 14
keeps retiming and recovery holds visible but leads with the applied input
buffer to match the graph. The later clarification gives extra its own row,
e.g. `Extra delay: 1.20 ms (smoothing/recovery) | Planned total: 9.82 ms`
with an input buffer of 8.62 ms.

| All 18 captures, Reduce judder on | 183 | 184 | 187 | 188 | Current |
|---|---:|---:|---:|---:|---:|
| Display jerk > 2 ms (per mille) | 23.0 | 18.1 | 16.7 | 18.5 | 17.4 |
| Display jerk p90 / p99 (ms) | 0.76 / 2.82 | 0.65 / 2.73 | 0.59 / 2.60 | 0.91 / 2.55 | 0.62 / 2.57 |
| Decode-to-display p50 / p95 / p99 (ms) | 4.59 / 8.15 / 10.17 | 4.52 / 7.84 / 9.68 | 4.76 / 8.09 / 10.37 | 6.82 / 10.76 / 12.27 | 4.36 / 7.69 / 9.19 |
| Queue-inclusive delay p50 / p99.9 (ms) | 7.63 / 17.36 | 7.73 / 17.35 | 8.05 / 18.34 | 9.80 / 17.23 | 7.84 / 16.02 |
| Overlay-sampled buffer / total max (ms) | 8.59 / 19.52 | 9.18 / 19.30 | 13.87 / 21.61 | 14.82 / 21.55 | 8.79 / 16.92 |

Every value is weighted by each capture's presented-pair count. Per
capture the current policy wins 100, ties 41 and loses 57 of 198
stat comparisons with daily.183. The losses concentrate on Smooth sessions,
where daily.183's fixed ~9.4 ms buffer cost 2.6-5.4 ms more median latency.
Absorb microstutter on the same base (reserve <= 5 ms at p99.5, 8 ms lag):
display jerk > 2 ms 14.0 per mille (p99 2.34 ms), decode-to-display
p50/p95/p99 4.81/8.36/10.18 ms, overlay buffer/total maxima 8.41/17.29 ms.
daily.188's Absorb reached 9.0 per mille only with 8.26 ms median latency and
a 15 ms buffer maximum; halving the miss allowance as well reached 12.6 per
mille but raised the buffer maximum to 10.8 ms and would silently retarget
the profile's coverage.
Replay keeps recorded admission and GPU/native execution: it cannot model
GPU contention, compositor behavior or physical scanout, the captures are all
Deck/Linux worker-paced (native scheduled Windows presentation is not
validated by this study), and live smoothness needs a fresh session. The
harness, per-capture configs and JSON are in `build/vrr-regress-20261016/`.

Main input-accounting correction (2026-10-12): current bounded-catch-up sessions serialize
`playout_input_clock_relative_history=3` and
`playout_input_bounded_recovery_credit=1`; absent fields keep historical captures
exactly replayable, and values 1 and 2 replay earlier versions of the correction.
Retained input requirements share a source-clock coordinate and are projected
into the current mapping for demand. The coordinate is the applied playout
offset minus host/client clock drift and stays continuous across a sender-clock
reseed. Mode 3 estimates drift as the least-squares slope of the offset over the
trailing 120 s (100 ms samples, refit each second, at least 15 s span). The
applied offset is a windowed minimum: on the 2026-10-12 23:04 Deck session it
held still on about 90% of frames and slewed in +/-20-27 us/frame bursts both
ways, while its trend was +32 ppm. Mode 1 used the raw offset, so drift moved
every retained requirement by drift x age. Mode 2 took the median of per-second
slopes, which read zero on that session and behaved like mode 1. Replaying that
capture with only this field changed (99.50% / 120 s profile): legacy 0.91%
misses at 3.44 ms mean buffer, mode 1 2.63% at 2.54 ms, mode 2 2.60% at 2.54 ms
(the live overlay showed about 97% coverage with a flat buffer), mode 3 0.56% at
3.43 ms. `tst_inputaccounting` covers smooth drift (+/-50 ppm) and a bursty
offset from sparse early arrivals (+/-30 ppm) against the legacy accounting.

Rate-drop follow (2026-10-12, `playout_input_rate_drop_snap=1` in current
sessions, absent in older captures): when the input layer switches to a period
at least a quarter longer than the last settled layer (116 to 60 or 30 FPS, 60 to
30 FPS), the input buffer takes its target directly, in both directions, for at
least 1 s and while the applied playout offset is still slewing toward its
window minimum, at most 10 s. The fitted period ramps through short-lived layers
during a drop, so each re-arms against the settled one; any other layer change
ends the window. The step coincides with the cadence change. On the 23:04
session the offset rose 9.3 ms over 4 s after a 105-to-30 FPS drop while the
source-relative target fell by the same amount; draining at 250 us/s carried
offset plus buffer from 8.8 to 17.3 ms for about 25 s. Replay of that session
(mode 3 accounting): offset plus buffer stays at the evidence level (about
10.3 ms), decode-to-submission mean over the next 15 s falls from 9.66 to
6.04 ms, and the whole session moves from 0.563% misses / 3.43 ms mean buffer
to 0.583% / 3.34 ms with presented jerk p99 3.42 to 3.44 ms. In the first 5 s
after the session's 110-to-30 FPS drop, jerk p95 rose from 16.2 to 17.7 ms.
Historical
coverage outcomes keep their original protection. Input recovery credit is
bounded by the configured interval allowance as well as physical VRR room;
PhaseTrajectory still uses physical room and its existing gradual repayment.
Profile target/history/tolerance, producer clock estimator, conditional decode
hold, rate layers and 250us/s release remain. Legacy unbounded catch-up retains
its original accounting. The former `MOONLIGHT_VRR_INPUT_ACCOUNTING` diagnostic
switch is retired; historical controls remain available in recorded replay
parameters. Tests and frozen capture guards establish software behavior; live
GPU/compositor behavior requires a coordinated comparison.

Absorb microstutter recovery (2026-10-14, superseded 2026-10-16): the recovered source-gap smoother
records revision 5. Reduce judder alone corrects the excess beyond the configured
interval tolerance within a phase limit of max(2 ms, 12% of the fitted period).
Absorb adds a standing reserve of min(2 ms, 15% of the fitted period) and uses
max(8 ms, 20% of the fitted period) for its phase limit. The previous 4 ms
Absorb limit clipped 763 of 15,577 adjacent submitted-frame decisions in the
latest completed capture, leaving short source displacements partly uncorrected.
The 8 ms limit changes correction room, not the standing reserve, frame admission,
profile buffer allowance, source-rate detector or presentation floor. Captured
parameters retain the old limit for exact replay; the calibration identity
includes the new limit so incompatible cached profiles cannot cross-seed it.
This recovery excludes the stash's separate decode-hold and LFC experiments.

Validation selected `/home/deck/moonlight-logs/moonlight-pacing-20261014-211054-23716.vrrtrace`
(5,187,557 bytes, last write 2026-10-15 02:16:12.575886 UTC, SHA-256
`57580ed4d55001c6ac9eb574c1b575501f3bff4a56d8b08d12863b0108039b60`).
Its clean-close footer accounts for all 15,677 rows. Fresh recorded-policy
baselines before and after the final rebuild both exit zero with valid sequence
integrity and `baseline_exact=true`. The candidate uses the full captured
controller snapshot with only `playout_smoothing_max_lag_us` changed to 8,000;
a partial override over generic defaults is not the matched comparison.

| Matched replay metric | Stashed 4 ms | Fixed 8 ms |
|---|---:|---:|
| Presented interval-change pairs over 2 ms | 53 per mille | 12 per mille |
| Presented interval-change p99 | 4.133 ms | 2.210 ms |
| Decode-to-submission mean / p50 | 16.541 / 15.506 ms | 14.784 / 13.817 ms |
| Decode-to-submission p95 / p99 | 24.207 / 32.481 ms | 23.337 / 31.818 ms |
| Decode-to-submission p99.9 / p99.95 | 34.858 / 36.811 ms | 36.777 / 37.109 ms |
| Sender-spacing residual p99 | 4.325 ms | 7.736 ms |
| Sender-relative jerk p99 | 6.204 ms | 8.398 ms |

The deliberate source retiming improves presented cadence while reducing sender
timestamp fidelity; it does not establish an abnormal source stall. The 6/8/10/12
ms sweep puts the useful knee at 8 ms. Ten matched nominal, periodic decision,
preparation, submission and two-frame scheduler-burst scenarios pass their
interval-safety and latency assertions without worker saturation. The nominal
p99 bound is 33 ms and the injected-fault bound is 45 ms. A heavier three-frame
4+4 ms wake injection saturates both policies and is excluded from cadence and
latency conclusions. The deterministic 6 ms displacement regression reduces
maximum presented interval change from 2,501 to 1,001 us on both presenter paths.
All fourteen selected VRR suites and replay help pass after the final diagnostic
build; the native app build, offscreen help and Desktop launcher help pass.

Replay retains admission and GPU/native execution costs. It cannot verify GPU
contention, compositor improvement or physical scanout. Raster bounds remain
unclassified on this Vulkan capture. Live visual improvement needs a fresh
session; an already running executable retains its old image. Complete configs,
test logs, baseline/candidate JSON, tail metrics, raster bounds and binary hashes
are in `build/absorb-fix-20261014/validation.json` and its sibling artifacts.
