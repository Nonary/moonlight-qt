# Streaming, VRR, and timing architecture

This is the persistent technical orientation for this fork. Read it at the start
of a session working on streaming, decoding, rendering, VRR, latency, or replay.
It explains the implementation and the reasoning needed to investigate it;
it does not establish that a particular deployed executable matches the source.

Current source review baseline: `3cb67d3e` plus the packet-count shimmering
warning correction in this worktree (2026-10-05). Automatic Windows composition
presentation, explicit native synchronization and continuous D3D11 overlay
publication are included.
Buffer control was reviewed against `244779f9` plus this worktree on 2026-10-07.
Production again selects responsive revision 7: client timing and buffer
growth, holds and release share the former one-second mean-before-tolerance
calculation. The displayed score and target therefore use the same metric.
Revisions 9/10 remain available only for replay of their recorded policies.
The experimental buffer policies from earlier tonight are stashed, not active.
The upstream two-step PyroWave calibration targets, planned-present timing
judgements and below-VRR-floor pause are included. The shared customizable VRR
settings, Reduce judder readiness bound, above-target shrinkage correction,
and the restored averaged interval scoring remain active. Deployment and live smoothness
must be verified separately from this source description. Dated investigation
sections below retain their historical policy and validation context; sections
3, 8, 11, and 13 describe current selection and execution.

macOS now has a native Metal presenter for the shared VRR worker. Session
eligibility uses the actual window's `NSScreen` refresh range, including
discrete ProMotion ranges, rather than assuming that SDL's current mode is the
display maximum. Metal keeps synchronized presentation enabled and requests
the native minimum visible duration for each drawable. OS-reported drawable
presentation timestamps are diagnostics, not optical scanout measurements.
See section 11 and [Mac build and use](docs/macos-vrr-pyrowave.md) for the
backend contract, dependencies, and validation boundaries. The first external
LG TV capture exposed drawable starvation and failed native cadence matching;
the resource correction alone does not establish working display adaptation.

The 2026-10-03 scheduling work also adds dedicated-video-thread priority
requests and CPU-pause polling in the bounded final deadline wait, described
in section 7.3. Controller targets and buffer policy are unchanged by that work.

The first live Windows PyroWave retry negotiated H.264 because the common library
had format constants without DESCRIBE selection or ANNOUNCE attributes. The
2026-09-25 4K/116 FPS PyroWave session subsequently confirmed live streaming.
Its Smooth buffer began at 8.189 ms (95% of the source period) and reached
14.343 ms: 72 recorded late-readiness growth decisions added 6.205 ms and only
0.051 ms was released. The captured policy required 10 seconds of uninterrupted
eligible observations before release, then drained at 50 us/s. This capture had 411
presented interval-sequence breaks (249 frame-number gaps, 112 phase changes,
49 other breaks, plus the initial row); only five spans reached 10 seconds.
The retained growth was largely a client release-policy effect, not a measured
14 ms decode or render cost. Growth frames had a 2.26 ms median first-packet
offset after the mapped source slot versus -5.20 ms in a regular-frame sample,
and 9.81 ms median packet assembly versus 8.33 ms; GPU completion after decoder
output was 1.81 ms versus 1.17 ms. The 900 Mbps session had 0.44% network
frame loss. This points to delivery/reassembly timing as the main cause of the
extra buffer without separating host send pacing from network transit. The
trace has a valid clean-close sequence and exact controller diagnostics, but
fails the strict replay baseline/raster readiness gate; it cannot support
strict counterfactual A/B claims.

PyroWave decode path (2026-09-26, source baseline `afd4aebb`):
the `PyroWave` codec choice negotiates Themaister's intra-only wavelet codec
(protocol in [docs/pyrowave-protocol.md](docs/pyrowave-protocol.md)). It reuses
`FFmpegVideoDecoder`, the pacer, VRR worker and stats, but no FFmpeg decoder.
`submitDecodeUnit()` parses each record- or length-prefixed frame through
`PyroWaveFraming`, pushes the wavelet packets and decodes on Vulkan. An eligible
partial frame can render with missing detail as blur; a rejected frame is dropped
without an IDR request because the next frame is independent.

PyroWave independent compression (2026-10-01, reviewed over `9a9dda86` plus
this worktree): the compression setting and session opt-in have been removed.
Moonlight uses ordinary PyroWave transport and removes saved `pyrowavecompression`
and `pyrowavehybrid` preferences. The decoder defaults to compression disabled.
Compression version 1 and feature `0x8` remain in the shared protocol code and
framing tests; they are not requested by production sessions. The abandoned
Hybrid `0x4` feature, frame-reference wire format and ACK control path are removed.

When another client requests the retained compression contract, the host encodes
a complete intra frame, preserves its raw coarse-data prefix, and packs detail
into independent groups of at most 64 KiB using fast LZ4. A 4 KiB
sample avoids full passes on high-entropy groups. Incompressible groups use native
records; if repacking erases the gain, the original framed bytes are sent unchanged.
Compression failures also send the native frame, including native framing's
unpadded fallback at the transport ceiling where there is no protected prefix.
No temporal comparison, XOR residual, reference cache, frame identity or ACK remains.
Deterministic sign/alignment padding stays because it improves compression entropy.

A compressed group has a 16-byte size/CRC32C header, begins on an RTP shard
boundary, and expands to native detail records before GPU submission. The framing
parser owns reusable expanded storage; each span identifies the original wire or
that storage. It validates geometry, sequence, detail-only records, exact LZ4 output
size and CRC before the ordinary clear-per-frame GPU decode. Packet loss skips only
affected groups, with record-start flags allowing recovery after a lost header.
An intact coarse prefix still displays a partial frame; one lost shard can remove
up to 64 KiB of detail. Intact output preserves every native coefficient exactly.
Critical FEC is unchanged; optional detail FEC observes native records before
compression and protects the resulting wire shards.

The 2026-09-30 frozen session delivered 1,884 partial frames and rejected 1,867 in
the old Hybrid path, producing zero rendering FPS. An active receive-side 1 Gbps
IFB test on the Deck's 2.5 Gbps NIC had cumulative drops; this is packet-delivery
loss and exposes Hybrid's whole-frame requirement. The replacement's regression
covers sustained detail loss and subsequent complete restoration. Neither those
tests nor a modeled wire benchmark establish live streaming smoothness.
The shared contract is in `pyrowave/compression/README.md` and
[docs/pyrowave-protocol.md](docs/pyrowave-protocol.md).

PyroWave partial-frame delivery (2026-09-26, `afd4aebb` plus the receive fix):
the RTP queue previously held an incomplete final block until the next frame
arrived. In capture `20260926-061208`, all 66 buffer increases in the first
90 seconds were attributed to final-block-loss frames; their median assembly
time was 8.806 ms versus 3.785 ms for intact frames. This is delivery lateness,
not a packet-loss counter in the VRR controller. The controller parameters
matched the earlier capture.

For PyroWave only, a final block without parity has a 1 ms packet-silence
deadline after its final data packet has arrived, renewed by unique accepted
packets. The final-packet requirement was added on 2026-09-27 (see below). The receive thread drains the
nonblocking socket before servicing that deadline. On expiry it can fill holes
and deliver only when the record-framing header announces a nonzero critical
packet count and that entire prefix is present, including FEC-recovered data.
Unknown framing, missing critical data, parity-bearing blocks and absent whole
blocks retain boundary-based recovery. An absent final data packet also retains
boundary-based recovery; silence between host batches cannot close that tail.
The deadline bounds reordering of interior detail after the end has arrived,
so interior packets arriving after the deadline may contribute blur. Reassembly and
decode timestamps remain actual event times. Other codecs and VRR calibration
thresholds are unchanged. Queue and native UDP tests cover completion, reordering,
critical-data protection and draining buffered datagrams. A fresh live capture
is needed to measure the resulting buffer behavior; existing replay starts
after reassembly and cannot simulate this receive-policy change.

PyroWave on-time release (2026-09-26, after `ce818ec9`): capture
`20260926-162709` (1440p116, 895.5 Mbps, Low Latency) delivered 16,162 of about
17,400 frames with lost packets, 14,029 of them by the silence deadline. At equal
packet counts those frames finished reassembly about 1.6 ms later than frames
completed by the next block boundary (6.26 vs 4.69 ms median at 250-300
packets): the 1 ms interval is rounded up to a whole-millisecond `WSAPoll`.
Measured against `sourceTime + playoutDelay`, 15.6% of them completed decode
late versus 2.5% of intact frames; 271 of 279 submissions more than 1 ms past
target were lossy frames. Presented jerk above 2 ms was 155 per mille for pairs
touching a lossy frame versus 58 for intact pairs. The session overlay reported
0% network frame loss because partial frames count as delivered.

The receive thread now asks the client for each frame's on-time bound through
`LiSetVideoReassemblyDeadlineCallback()`, once per final block. The VRR worker
publishes, after each `schedule()` under timestamp playout, the anchor
`sourceTime + playoutDelay - p95(decodeComplete - reassembled) - 250 us` for that
frame's RTP timestamp (`vrr/receivedeadline.h`: one packed atomic word, rebuilt
relative to now, extrapolated at 90 kHz, ignored beyond one second of RTP
distance, cleared when timestamp playout is inactive and on worker shutdown).
Without cadence smoothing this matches the lateness the delay calibrator sees.
When that bound is nearer than the 1 ms silence, the silence shrinks to
`max(bound, lastUniquePacket + 250 us)`, and the receive loop wakes a
millisecond early and polls without blocking so the release is not rounded
late. A frame with gaps shorter than 250 us stays open because each unique
packet renews the floor. Longer paced gaps can expire a late frame while the
host is still sending it: the host sends in 1 ms pacing groups, and a
deterministic zero-loss 800 us gap caused the queue to mark and reject two
later detail packets. This can create blur without network loss. It is separate
from the sustained decode backlog measured in the 2026-09-26 4K capture:
only 661 of 127,960 decoded frames were partial, while intact frames still
took 10.05 ms per decode call on average.
The same critical-prefix, parity and final-block conditions apply, and such
releases log as "on-time deadline" instead of "packet silence". The controller
is unchanged (replay of `162709` is identical). A bounded Desktop capture on
2026-09-27 observed 61 partial frames out of 410, all delivered at the on-time
deadline; it cannot distinguish true network drops from early expiration.


PyroWave receiver completion correction (2026-09-27): the latest six-minute
1440p120 4:4:4 10-bit capture had 400 partial-delivery events, all in the final
block; 398 ended after multiples of 46 data packets, matching the sender's
64 KiB batch limit with default packet sizing. Native UDP tests reproduced
153 synthesized holes and 153 rejected suffix packets despite all 199 final-block
datagrams reaching the socket. A late-frame 800 us gap triggered the short
expiry; a 2500 us gap triggered ordinary expiry. Parity-marked blocks avoided
the cutoff without invoking FEC recovery, because they bypass silence expiry.

The queue now arms and services silence expiry only when the highest received
sequence equals the block's final data sequence (for zero-parity blocks).
This uses existing sequence tracking, including wrap and reordering, rather
than trusting an EOF flag alone. Intact frames still submit immediately;
interior holes after the final packet retain bounded reorder recovery. An
absent tail waits for completion or a successor boundary, so a genuinely lost
final packet may add up to the wait for the next frame before partial delivery.
Regression tests cover spaced 46-packet batches, late/near/unknown deadlines,
sequence wrap, misleading EOF flags, successor recovery and interior reordering.
These tests establish receive-path correctness; a new live capture is still
needed to measure visual improvement and actual loss after this change.

PyroWave loss costs detail, not timing (2026-10-06). A USB 2.5GbE dock drops
whole 12-16 packet USB transfers, frequently including a frame's final packets;
waiting for the successor delivered those frames 5+ ms late and the pacer then
grew its buffer for them. With the final packet absent, the queue now arms
`PYROWAVE_TAIL_SILENCE_US` (3 ms) or the VRR slot, never sooner than
`PYROWAVE_LATE_TAIL_SILENCE_US` (1.2 ms) after the last unique packet; both
exceed the 800 us and 2.5 ms host gaps above, whose regression still passes.
Critical-prefix and parity rules are unchanged. `FFmpegVideoDecoder` carries
each frame's `BUFFER_TYPE_LOST` count to `PacedFrame::lostPackets()`, and
`VrrTimingController::schedule()` clears that frame's smoothness and prediction
eligibility: it is presented as scheduled, but its lateness never grows the
readiness, interval, mean-miss or smoothness buffers. A 237 s 740 Mbps capture
before this change had 267 partial frames and 11.8% of presented intervals
jerking over 2 ms.

On Windows, `initializePyroWave()` creates `D3D11VARenderer` and a PyroWave
Vulkan device matched by adapter LUID. Decode submits into one of ten D3D11-owned
three-plane R8/R16 surfaces. The software-planar-format `AVFrame` holds a
`PyroWaveFrameRef` with the surface and decode fence value; two shared D3D11
fences carry decode completion and renderer release. `renderPyroWaveVideo()`
queues `Wait()` on the decode fence, draws the planes and signals the release
fence. `decoderOutputUs` is decode submission time, while VRR `waitForDecode()`
observes completion. `captureDecodeBoundary()` stays 0 (single device).

On Linux x86-64 with libplacebo, `initializePyroWave()` uses `PlVkRenderer`,
which requests the decoder's Vulkan 1.2/1.3 features (subgroup size control,
timeline semaphores) and owns a `PyroWavePlaceboPool`. The decoder creates its
PyroWave device on the renderer's own `VkDevice` (`pyrowave_create_device`,
shared queue families, queue submissions serialized through libplacebo's queue
locks) and decodes into R8/R16 plane textures lent by the pool. For each frame
the pool calls `pl_vulkan_hold_ex()` into `VK_IMAGE_LAYOUT_GENERAL`, signalling
a timeline value the decode waits for; the decode signals the next value, and
`pl_vulkan_release_ex()` makes libplacebo wait for it before sampling. The
decoder thread only submits work. The pool also exposes libplacebo's separate
compute family when available, but the decoder now retains PyroWave's default
graphics queue. The 20260927-194622 final connection (4K 4:4:4 10-bit) showed
Wayland presentation delays frequently tracking completion of the next async
decode despite the displayed image being GPU-ready before submission. Returning
from Present does not establish compositor completion. The graphics-queue
rollback is a candidate mitigation for this contention, requiring a new live
capture; it is not a measured smoothness fix. It can also serialize later
rendering behind decode. An earlier headless queue A/B showed no measurable
render-completion difference and did not exercise compositor presentation.
Plane textures retain concurrent sharing across libplacebo's families, so no
ownership transfer is needed. The `AVFrame` carries the pool reference in
`buf[0]` with a `YUV420P`/`YUV444P` (8-bit) or `YUV420P16`/`YUV444P16` (10-bit)
format for colour metadata; `mapAvFrameToPlacebo()` builds the `pl_frame` from
the pool textures instead of `pl_map_avframe_ex()`. Freeing the frame returns
the surface; the next hold orders the rewrite after libplacebo's pending reads.
Up to eight surfaces exist; with none free the frame is dropped. If the device
lacks the features or sharing fails, the decoder falls back to a private device
and synchronous readback into 16-bit planar frames that libplacebo uploads.
Linux `decoderOutputUs` is decode submission time on the shared path. Each
pool-backed frame retains its own decode timeline-semaphore value; the Vulkan
renderer waits for that value before the VRR worker schedules the frame. The
wait is bounded to 50 ms, holds no pool/queue locks, and requests renderer
recovery on timeout or device failure. The existing worker records meaningful
waits (over 200 us) as a conservative completion observation while preserving
the immutable decoder-output timestamp. Already-ready frames do not claim a
new completion time. For asynchronous PyroWave output, receive/flip protection
learns only from observed completion, so CPU submission durations cannot
overwrite measured GPU cost. The decoder explicitly identifies asynchronous
output to the pacer; synchronous readback retains its valid output-completion
samples, and other codecs retain their previous sampling behavior.
The separate output-render wait remains in place for PyroWave's planar-format
frames; this correction does not enable asynchronous source retirement.
The GPU sidecar's `pyrowave_decode_sync` span records the CPU wait and its
Vulkan result, not a GPU execution timestamp. The
round-trip test decodes through both paths (shared planes read back with
`pl_tex_download()`) and checks PSNR in 4:2:0/4:4:4 at 8/10 bits; it does not
establish live-stream cadence or physical scanout. VRR policy and replay are
unchanged.

On macOS with the bundled MoltenVK driver, `initializePyroWave()` creates the
native `VTMetalRenderer` and requires shared GPU output. Its
[`PyroWaveMetalPool`](app/streaming/video/pyrowave/pyrowavemetal.mm) creates a
Vulkan 1.2 device whose exported `MTLDevice.registryID` matches the presenter's
Metal device. It requires timeline semaphores, subgroup-size control/full
subgroups, and `VK_EXT_metal_objects`. Granite retains the reported native
subgroup width on MoltenVK when required-size pipeline stages are unavailable;
it does not fabricate required-size support or change other drivers' selection.
The embedded SPIR-V 1.3 shaders use that fixed native width without an
allow-varying flag. The local Apple M5 Pro reports 32 lanes.

The pool lazily allocates up to eight three-plane R8/R16 Vulkan images and
exports their actual Metal textures. An initial ready timeline orders the
transition to `GENERAL`; decode uses the shared Vulkan graphics/compute queue,
with queue submissions locked, and signals a per-frame done timeline value.
The planar-format `AVFrame` contains a pool reference and that value in `buf[0]`
rather than CPU pixel buffers. The Metal renderer samples those same textures;
there is no readback/upload step. With all eight surfaces held, the frame is
dropped. Unsupported device features or failed sharing make Mac PyroWave
initialization fail; production does not silently use the synchronous readback
fallback available to Linux and the regression harness.

Mac PyroWave output is asynchronous. Before VRR scheduling, the renderer polls
that frame's timeline and waits up to 50 ms if needed, holding no pool or queue
lock. Failure requests renderer recovery and prevents adaptive preparation.
An already-complete value does not invent a new completion time; meaningful CPU
waits supply the same conservative completion observation used by the shared
worker elsewhere. Immutable decoder output remains the latency origin.
Rendering retains the `AVFrame` until Metal command completion, so returning a
surface to decode cannot race a Metal read. Frame references retain the shared
Vulkan device/images/memory and Metal textures even after pool teardown. The
existing bounded output-view cache is used only with this immutable surface
geometry; decoder teardown precedes replacing those output images.

Native regression reads shared output through Metal blits and compares every
byte with synchronous Vulkan reconstruction. It covers chroma/bit depths,
poisoning/reuse, pool exhaustion, per-frame completion and retained lifetimes.
These are ownership/reconstruction checks, not host-stream throughput or
physical presentation proof. Mac Metal calibration and validation boundaries
are described in [Mac build and use](docs/macos-vrr-pyrowave.md).

PyroWave coefficient-store optimization (2026-09-27): RADV specializes the
dequant shader to stage each 32x32 coefficient tile in 4 KiB of FP32 shared
memory. A workgroup barrier precedes writes in contiguous lane order, replacing
the scattered per-lane 4x2 store pattern. Every coefficient, image format, and
reconstruction operation is retained; this does not enable FP16 arithmetic.
Missing tiles are zeroed directly in contiguous order without a shared-memory
barrier. Other drivers retain the original stores through specialization ID 0.
`scripts/regenerate-pyrowave-dequant.py` refreshes only the three dequant SPIR-V
variants and their specialization reflection in the vendored shader bank.

The final headless Deck A/B/B/A comparison at 3840x2160 4:4:4 10-bit, using the
same three encoded test patterns and normal dynamic power management, measured
mean decode completion of 11.00 ms before versus 8.11 ms after (120 timed frames
per run, two runs each). Earlier prototype comparisons measured 8.22 versus
6.37 ms; absolute timings vary with device conditions. All 149,299,200 output
bytes across the three final decoded images matched the original decoder.
The coefficient test checks both store paths, missing and clipped tiles,
positive/negative values, R16/R32 storage, and storage-buffer/texel-buffer input;
eight hardware cases and four default software-Vulkan cases passed. The complete
round-trip suite also passed. These checks do not establish sustained 120 FPS
with rendering/composition or long-session stability. The final timing runs
still included decode calls longer than the 8.33 ms frame period.

PyroWave client overhead reduction (2026-09-27, over `ccf21a1e`): the shared
Windows/Linux framing parser checks packet loss once per frame, skips packet-map
searches when intact, advances a cursor for forward record traversal when damaged,
and retains its span allocation across parses. Record validation and loss recovery
remain in place. A bounded 820,024-byte / 2,500-record / 684-packet microbenchmark
measured about 225 us before versus 15 us after; 10,000 differential cases covering
loss, padding, corruption and reused state matched the original parser.

The Windows and Linux shared-surface paths opt into a bounded C API output-view cache.
The cache retains Vulkan image views for up to 16 distinct three-plane output
descriptions, keyed by the image handle and every view field; additional outputs
use transient views. The Windows ten-surface and Linux eight-surface pools keep
their images alive until decoder teardown, which precedes renderer teardown.
The cache defaults off for other C API callers and stays off for Linux synchronous
readback. It changes neither image ownership nor acquire/release synchronization.
Recreating an external output image requires disabling the cache first (or
destroying the decoder), even if Vulkan later recycles the same image handle.

An alternating off/on/on/off headless Deck comparison at 4K 4:4:4 R16, paced at
120 FPS, measured CPU preparation/submission excluding context wait at 233 us
uncached versus 209 us cached. Every output sample matched (full-plane hash).
GPU completion averaged roughly 8.4-8.5 ms in both modes, with no established GPU
throughput improvement. These synthetic results use an 8-bit source encoded to
R16 output, not a live HDR or physical-scanout test. The round-trip regression
rotates and poisons all eight Linux output surfaces, checks full output bytes,
and covers 4:2:0/4:4:4 and R8/R16. Windows runtime performance is unverified.

moonlight-common-c asks for an 8192-packet receive buffer on PyroWave video
sockets. Linux silently clamps SO_RCVBUF to `net.core.rmem_max` (208 KB by
default on SteamOS), and the kernel drops packets that overflow it; the library
logs a warning when the buffer falls short. `NetworkBuffers` reads the limit:
Settings shows a warning under the PyroWave codec options with a button that
runs `pkexec` on the host (through `distrobox-host-exec`/`host-spawn` inside a
container) to set 32 MB now and in `/etc/sysctl.d/60-moonlight-pyrowave.conf`,
plus a copyable command. Flatpak builds cannot run host commands, so they offer
only the command. Launching a PyroWave stream with the low limit adds a launch
warning.

On Windows the same `NetworkBuffers` checks the receive rings of every
connected wired adapter (2026-09-26): the standard NDIS `*ReceiveBuffers` and
the Realtek USB driver's `ReceiveBufferLen` / `PendingReceives`, read from the
adapter's class key with ranges from `Ndi\Params`. A value below
min(driver maximum, 256) is flagged. The Realtek Gaming USB 2.5GbE on the test
Ally shipped with 16 of 256 receive buffers and 6 of 64 receive URBs; in
capture `20260926-143554` 72% of PyroWave frames lost ~24 packets, almost all
in the last FEC block (the tail of each ~2 Gbps burst), while Windows counted
no discards, and still images shimmered as the missing detail moved. Settings
shows the adapter and values with a "Fix it" button that raises each flagged
value to its maximum (capped at 2048) through one elevated PowerShell
(`Set-NetAdapterAdvancedProperty -NoRestart`, then one `Restart-NetAdapter`),
plus a copyable command; streams launch with a warning. Whether raising the
ring removes the loss still needs a capture after the change.

The "Average decoding time" statistic runs from the reassembled frame's
enqueue in moonlight-common-c to decoder output, so it includes time waiting in
the 15-frame decode-unit queue; the wait is shown separately. When that queue
overflows it is flushed and an IDR is requested, which restarts cadence.

PyroWave decode attribution (2026-09-27, over `8e95a0b4`): deep GPU diagnostics
add three events to the independent GPU CSV, without changing replay or pacing.
`pyrowave_phases` uses `a..e` for CPU wall microseconds spent parsing,
pushing/validating packets, acquiring/allocating output, submitting decode (or
synchronous readback), and releasing/referencing output. Its object field is
decode success; failed calls retain the phase in which they failed. With compression
the parsing phase also includes CPU group validation/decompression. These
durations include any resource-reuse waits and are not GPU execution times.
`pyrowave_context_wait.a` isolates the CPU wall time inside Granite's
`next_frame_context()` from the larger submit phase. This advances one of two
codec frame contexts and can wait for previously submitted GPU work; the
timing is zero on non-shared/readback paths.
`pyrowave_payload` records framed bytes in object, and surviving payload bytes,
received block records, announced blocks, stripped padding bytes and partial
status in `a..e`. With compression object is the received wire size, while
payload/blocks describe surviving native records after group expansion. At teardown, a decoder
that recorded phase diagnostics logs
the codec's existing GPU history, including Dequant and iDWT durations per codec
frame context, before imported-image teardown advances extra contexts. Those
are aggregated delayed GPU query results, not per-frame completion timestamps.
The diagnostics are intended to distinguish a change in received content from
GPU backpressure when the host changes; they do not establish host-OS causality
from the older aggregate `packet_send` span alone.

PyroWave calibration (2026-09-26, over `41312909`): select PyroWave in
Settings > Video codec to show Calibrate PyroWave. The codec selector and
YUV 4:4:4 checkbox sit above the bitrate controls. Calibration grades, at the selected FPS, 4K, 1440p, 1080p, 720p and (Linux)
Deck-native 800p, each in 4:4:4/4:2:0 and HDR (10-bit)/SDR (8-bit). The worker
refuses to run during a stream, reports each format as it finishes, and stops
within a frame when the dialog closes (a format cut short has no result). For
each probe it encodes one synthetic image at the bitrate's per-frame budget and
runs the pipeline saturated: decodes feed a three-frame queue drawn in order,
so the interval between two frames finishing is the second one's cost. On Linux
a second thread draws each frame with `pl_render_fast_params` into a target the
size of the dialog's screen (`rgb10a2`/HDR10 for 10-bit frames tagged BT.2020
PQ as a stream tags them, `rgba8`/sRGB for 8-bit) and waits with a one-pixel
download. On Windows the one-pixel D3D11 decode-completion readback runs on the
decoding thread, nothing is drawn, and decoding may use three quarters of each
frame period. A short run (half a second of frames) warms clocks and shaders
and stops a format whose mean cost exceeds the period. A timed run of three
seconds of frames (at least 300) follows, and a format keeps up when its p99
per-frame cost fits the frame period. The grade does not depend on the VRR latency mode. A miss that isn't
plain overload is measured once more and the better run kept, because a system
stall can land in any run.

The default bitrate and calibration's author recommendation use the same
35 dB calculation rounded up to 5 Mbps, including the HDR allowance. The
default no longer applies a separate 900 Mbps cap and needs no calibration.
On macOS the local calibration probe uses the shared MoltenVK-to-Metal pool and
the native triplanar video shader, CSC/chroma helpers, and bit-depth scaling.
It renders a fitted video region into an offscreen target at the supplied
display pixel dimensions (BGRA8 for SDR, BGR10A2 for BT.2020/PQ HDR frames),
then observes a one-pixel readback. No window, drawable, or display link is created. Source
decode and Metal draw finish serially within a shared 50 ms readiness bound,
and grading reserves the same conservative quarter-period headroom as the
existing Windows serial check. The source frame stays retained by GPU
completion even on a timed-out probe. Network qualification and recommendation
policy are shared; this probe measures local decode/draw rather than compositor
presentation or physical HDR behavior.

Calibration (2026-10-02, reviewed against client `92119295` and host
`6d8fc5ac` plus these changes) requires an online paired PyroWave host advertising
UDP probe v1 and wire budget v1. Calibrate PyroWave opens a two-step modal card.
Step one selects the host and bitrate target and runs only the bandwidth test.
The 2026-10-04 UI follow-up gives the four target cards equal widths and short
budget descriptions, highlights the next useful action, and groups connection
progress and the confirmed budget in a status panel. The first screen is 480
logical pixels tall where space permits, with controller hints below the panel.
The 2026-10-05 correction reserves the spinner width and status text space so
starting a transfer does not reflow the panel. Step one scrolls when its content
exceeds the available height. Its result grades the resolution, FPS, chroma and
HDR settings selected when the test starts, using the confirmed image capacity
after FEC, packet overhead and the chosen target's wire allowance. Green means
the selected Minimum or Recommended guide is met (Moderate and Maximum compare
against Recommended); yellow means below Recommended but at least Minimum;
red means below Minimum. The target's
required wire rate is shown beside the measured budget. A usable lower-quality
connection can still continue to the per-format decoder tests, where an Any
display result is yellow/red when its image rate misses these quality bounds.
Four target cards default to Recommended: Minimum requests half the author's
recommended image bitrate (rounded down to the 5 Mbps step, with a 5 Mbps
minimum), Recommended requests the author's full image guide, Moderate aims for
60% of the slower known link speed, never below the format's Recommended wire
rate and stepped down to the freshly confirmed budget when the path cannot carry
it (`PyroWaveCalibration::wireTarget`; 60% of a confirmed 1135 Mbps on a
2.5 Gbps link had fallen below Recommended), and Maximum uses the full
confirmed budget. All four respect network, packet/FEC overhead, frame capacity
and device limits.

Explicit left/right handlers select and focus the target cards; controller
up/down maps through Tab/Shift+Tab to the host and test controls. Targets are
locked while a test runs. Next is enabled only after the network worker exits
successfully and the selected host/target still matches the tested combination.
It starts a separate decoder/render stress-test worker on step two, using only
that calibration's confirmed budget, FPS, display size and transport policy.
The backend also checks host/target identity before starting the decoder.
Failed or cancelled bandwidth cannot advance; closing the card cancels the
worker and invalidates its budget. Stop allows a running test to finish its
current transfer/frame. Back from step two returns to target selection once
the worker stops; cancellation requires a fresh bandwidth test.

Step two shows progress and the format grid. Choices become selectable after
the decoder worker finishes, with focus moving to the first available format.
Up/down navigation stays within a chroma column, left/right changes chroma while
retaining HDR/SDR, unsupported choices are skipped, and scrolling follows the
focused choice and window resizing. Focused format cards have an explicit outline.
Detailed explanations are under About results; valid slow formats remain
selectable and dimmed. Results from another host or target are hidden and cannot
be applied. The UI was exercised at 1280x800 and 800x600 using injected SDL
controller-button events through the production navigation dispatcher and a
staged calibration fixture; that does not establish physical controller or
live-host calibration behavior.

The network search starts with a 2-second paced UDP probe at its bounded
ceiling. For Maximum that ceiling is the smaller known endpoint link speed,
capped at 3 Gbps; Moderate stops at 60% of it or the matrix's largest
Recommended rate, whichever is higher, plus the confirmation margin; for Minimum and Recommended it is also limited to
the largest target in the format matrix, with room for applied bitrate rounding
and the final 5% margin. A failed ceiling is bisected to 5 Mbps precision. A
capacity candidate must have under 2% overall loss and under 5% in every 100 ms
window (PyroWave loss is blur; unsent packets count as lost), at least 98% of
packets sent on time by the host, <=2 ms growth and finite timing measurements.
A 0.1%/all-sent rule collapsed a 2026-10-06 run to 480 Mbps after the host
transiently failed to send 384 of 204,783 packets. Vibeshine now retries a full
send buffer for 1 ms and reports `sendRetries`/`lastSendError` per probe. The chosen rate leaves 5% margin where possible and passes two fresh
confirmations; failed confirmation backs off another 20%. Sequence accounting
includes lost tails without counting duplicates. The <=4 ms p99 relative transit
variation gate remains the separate strict stability grade, but no longer
prevents local GPU/format calibration on a loss-qualified, non-growing link.
The 2026-10-05 follow-up removes the 4 ms warning and its effect on result
colors from both calibration steps. Transit variation remains in diagnostic
logs and the measured-throughput summary. Result colors grade image quality.
Missing support, blocked UDP, persistent loss or growing queues give no
recommendation. The legacy bulk
HTTPS probe remains available but is not used to grade stability.

The UDP receiver resolves DNS/mDNS hostnames such as `ambidexl.local` before
binding an ephemeral IPv4/IPv6 socket. Lookup has a five-second bound and
observes cancellation. Dual-stack names prefer IPv4; numeric IPv6 and IPv6-only
names retain IPv6. The pinned-certificate HTTPS probe uses the same resolved
address as the UDP source filter, avoiding address-family mismatches. Resolution
and socket-open errors identify the actual failure. The original numeric-only
parser rejected hostname connections before sending any probe packets.
Windows calibration now requires `PyroWaveUdpHandshakeVersion=1` in addition to
the existing probe and wire-budget capabilities. Linux and macOS use it when
advertised and retain their legacy probe against older hosts. The client requests `handshake=1`
and handles the paired HTTPS response's `X-PyroWave-Udp-Port` header while its
XML body remains pending. It sends the 32-byte token from the actual receiving
socket to that announced host port, retrying every 100 ms until an authenticated
measured packet arrives or cancellation occurs. Measured packets must match the
announced IP and port as well as the token and sequence. The matching host opens
outbound state with ungraded token warmups, allows at most 1500 ms for the token
from the HTTPS peer's IP, and starts its two-second measurement afterward. This
establishes the UDP return path on both peers without changing firewall rules;
HTTPS alone cannot establish that UDP exchange. Warmups do not enter the packet
counts or timing scores. On Windows, legacy hosts produce an explicit update requirement.
The host implementation must be deployed separately before live calibration
can be verified; a passing client loopback test does not establish live success.
Authenticated UDP-test HTTP errors preserve the host's plain-text rejection
reason in the UI and log. In particular, a host with an active stream refuses
calibration with HTTP 400 and asks the user to stop streaming first; this is
not a UDP reachability failure. Calibration does not terminate that session.

On macOS, calibration timing uses `SO_TIMESTAMP_MONOTONIC` ancillary packet
timestamps in the `mach_absolute_time()` domain. These mark socket enqueue,
before the application drains the queue; Qt event-loop/read scheduling delay is
reported separately and does not inflate network delivery variation. The
receiver peeks metadata, then consumes through Qt so address parsing and socket
notifications remain consistent. Missing native timestamps on authenticated
probe packets fail timing qualification explicitly. Other platforms retain
their existing read-time measurement. Apple's
[UDP input](https://github.com/apple-oss-distributions/xnu/blob/main/bsd/netinet/udp_usrreq.c)
and [control timestamp code](https://github.com/apple-oss-distributions/xnu/blob/main/bsd/netinet/ip_input.c)
define the enqueue timestamp's clock and placement. Delayed-reader regressions
exercise IPv4 and IPv6 reception and notifier rearming on the actual Mac kernel.
The failed search retains its last probe, and the UI reports its specific
loss, host-duration, or delivery-timing failure with measured values. Strict
stability limits and capacity confirmation requirements remain unchanged;
format support is no longer inferred from the strict jitter grade.

Calibration converts the confirmed total wire rate to image bitrate using the
host's critical FEC percentage, minimum parity, the selected packet size, packet
headers/rounding, and audio/control reserve. The shared bandwidth formula only
reserves parity for one critical block; adaptive detail parity consumes spare
cadence budget within the same total. GPU probes begin at the selected image target within the
network/frame-capacity bound. A passing target needs one complete measurement;
there is no upward staircase. If a bandwidth target fails, the author's guide
is tested as a lower bracket when applicable. Otherwise the existing floor
probe and minimum 10% GPU saving rule govern whether reducing image quality
is useful. At most six bitrate bisections refine a passing/failing device
bracket; every passing candidate retains the full timed run. The p99 threshold,
warmup, 300-frame/three-second minimum, system-stall retry, and two fresh UDP
confirmations are unchanged. Slow rows retain the actual target's timing when
no lower rate works, rather than attaching floor timing to the target bitrate.
Applied rates include FEC and headers; the quality estimate and tooltip use
image Mbps. Reduced/low quality stays visible even on slow-format rows.
`pyroWaveQualityDb()` retains its existing regression/inversion semantics.

At stream launch the client's known routed wired speed is sent in
`x-ss-video[0].pyrowaveLinkMbps`; the host uses the smaller known endpoint speed
for packet pacing. Its physical wire cap no longer applies a second 20%
reduction after calibration. The capacity probe uses evenly spread 1 ms groups.
Against hosts with `PyroWaveUdpProbeBurstVersion=1`, calibration then sends the
selected settings' bitrate (capped by the budget) as frames on the stream pacer's schedule and keeps the fastest
pace passing aggregate and per-frame loss checks (`PyroWaveLink::searchPace`).
The 2026-10-07 correction reconstructs each burst-v1 frame's sequence range
and its final 1 ms send group using the host's exact packet/wire accounting.
All missing packets, including absent frames and tails, enter the frame's
loss percentage. A speed fails if any frame loses **more than 10%** of its
packets, or at least 2% of frames have holes in the final send group. A
surviving final packet does not excuse holes elsewhere in that group. Smaller
interior holes are allowed within the aggregate under-2% loss and under-5%
per-100-ms limits; the share of frames with any hole is diagnostic only.
Both fresh confirmations must pass all checks. The search first tries the
link, then bisects a passing boundary with a measured 5% margin. If this path
fails, it checks remaining 50 Mbps steps across the usable range: a failed
floor does not prove that all intermediate speeds fail. If none qualifies,
it selects the best usable measurement, ranked by fewer violated limits,
fewer tail-damaged frames, fewer frames over 10%, worst frame loss, then
aggregate/window loss (faster on ties), and measures it twice afresh. Invalid
or incomplete probes cannot become a fallback. Failed confirmations remain
in fallback ranking rather than inheriting a lucky initial result. The result
and status explicitly warn when no tested speed meets all limits; this does
not automatically reduce the bitrate budget. Every format remains capped at
the actual frame-tested budget, even when aggregate capacity was higher.
Logs and the result summary distinguish aggregate loss, the share of frames
over 10%, missing tails and worst frame loss. Missing burst support/link speed
still prevents a recommendation. Passing does not promise zero artifacts.
Applying a format stores the selected pace as
`StreamingPreferences::pyroWavePaceMbps`, announced at launch in
`x-ss-video[0].pyrowavePaceMbps`. Uncalibrated, the host paces at 80% of the
link, because a USB 2.5GbE dock dropped whole frame tails above ~2.2 Gbps.
Settings exposes Packet speed (Mbps) beside video bitrate for PyroWave. Manual
values are editable in 50 Mbps steps (or directly typed), and zero/Automatic
restores the host's default. Applying calibration updates this same persisted
value; manual changes take effect on the next stream. The host still caps the
requested pace at the link and raises it to the frame's required rate, so the
saved value is a request rather than a measurement of actual wire speed.
Host encoding, sustained gameplay and physical scanout remain unverified. See
[protocol details](docs/pyrowave-protocol.md#fec-inclusive-recommendations-and-udp-calibration).

The color is smoothness risk, with margin for a live stream costing about a
third more than the test (4K 4:4:4 10-bit took 8.3-9.1 ms per frame while
backlogged live against a 6.7 ms test mean): green ("Any display") at a p99
cost of at most 60% of the period, yellow ("Needs VRR") up to 80%, orange
("Needs VRR · Smooth mode", the large buffer) up to the full period, and red
("Can't keep up") beyond it. Clicking a format applies the codec, resolution, chroma, HDR and the
bitrate shown, and turns off automatic bitrate. Every format is freshly tested;
no cached samples or preliminary passes replace the complete measurements.
Run duration depends on the chosen target and the number of device/link failures;
the former fixed "about two minutes" UI estimate is removed.

Local verification (2026-10-02, Deck idle): a matched 120 FPS GPU-only sweep
at a 950 Mbps confirmed wire budget and 1280x800 render target fell from
28.258 s on `92119295` to 14.018 s with the ceiling-first Maximum search.
All 20 formats retained the same selected image/wire rates and keep-up verdicts;
p99 timings and margin tiers remain fresh measurements and can vary between
runs. Minimum, Recommended and Moderate completed all 20 formats in 12.476,
13.278 and 12.829 s respectively. Each retained the complete timed measurement.
The application build, deterministic policy/budget tests, and production QML
selector/default checks passed. These times exclude UDP, pairing/HTTP setup
and real host encoding. UDP search speed is covered by deterministic probe-count
tests; complete live calibration and gameplay stability are not established
by this GPU benchmark.

Why this shape, on the Deck (Van Gogh, 4K HDR10 target, 116 FPS): decode cost
follows resolution and chroma, not bitrate. Decode alone for 4K 4:4:4 10-bit
took 7.75-8.16 ms per frame from 330 to 850 Mbps, so lowering bitrate rarely
rescues a format. With the render overlapped as in a stream, mean costs were
6.4-6.8 ms for 4K 4:4:4 10-bit (74-79% of the period), 5.6-6.2 ms for 4K 4:4:4
8-bit, 3.6-4.2 ms for 4K 4:2:0 10-bit and 3.3-3.8 ms for 1440p 4:4:4 10-bit.
Two sweeps on 2026-09-26 gave p99 costs of 9.4-10.3 ms for 4K 4:4:4 10-bit
(can't keep up), 8.4-8.5 ms for 4K 4:4:4 8-bit (near limit), 6.1-7.0 ms for 4K
4:2:0, 5.4-6.0 ms for 1440p 4:4:4 and under 4.6 ms for everything smaller; only
4K 4:2:0 8-bit changed grade between runs, at the near-limit edge. Live, 4K 4:4:4 HDR at 116
FPS held on 2026-09-25 but on 2026-09-26 the decode-unit queue overflowed every
1-2 s (decoder wait p90 42-120 ms, an IDR each time), while 4K 4:2:0 and 1440p
4:4:4 stayed clean; the synthetic test does not include presentation, network
receive, host cadence bursts or the Deck's shared CPU/GPU power budget, which
is why near-limit formats carry a warning.

Earlier measures were misleading. Grading the p99.95 lateness of a queue
replay against the VRR buffer let the buffer hide 4K 4:4:4 10-bit's overload
under Balanced, while one system stall in 2000 frames marked 1080p 4:2:0 and
800p 4:4:4 (27-31% of the period) near limit. Timing a paced run with the test's own
threads sleeping to each arrival put scheduler jitter into the tail: three
identical 4K 4:4:4 10-bit runs gave p99 latency of 51, 12.7 and 22 ms, while
the saturated replay's p99 cost was 9.26-9.30 ms. Drawing serially (decode,
draw, wait, decode again) measured 113-120% of the period for that format,
understating the overlap a stream gets. One paced decode plus one-pixel wait
per period overstated cost 2-3x because the GPU idled between frames (1080p
4:4:4 10-bit: 6.6 ms p95 against 2.6 ms back to back). Before that, a fixed
900 Mbps link cap and `sleep_until()` lateness treated as backlog marked every
Windows format slow. Host encoding, throughput beyond this device's link,
presented cadence and the picture quality of real content remain unmeasured:
the synthetic image fills the byte budget, and the dB grade is the author's
regression for the bitrate, not a measurement of the stream.

`HAVE_PYROWAVE` adds a member to `FFmpegVideoDecoder`, so changing that qmake
option requires `make -C build/app clean` before rebuilding the app. An
incremental link with pre-option objects (such as `session.o`) can compile and
pass `--help`, then corrupt the heap during the startup decoder probe.

Balanced readiness floor (2026-09-24), based on `fae3eefe`: the interval-quality
score averages absolute interval error over one second, which dilutes an
isolated 4 ms late frame ~100x. Capture `20260923-232347-186` (116 FPS Balanced,
4K HEVC on the 890M) scored 99.77% against the 99.5% target while visibly
hitching every few seconds, so the buffer never grew and released 8.2 -> 6.9 ms
although decode waits ran 6.4 ms p50 and 8.7-11.4 ms on the hitching frames.
Production Balanced then set `playout_readiness_floor_per_mille=500` and
`playout_readiness_floor_window_us=10000000`: `playoutDelayMinimumUs()` is at
least the median ready offset (decode completion after the mapped source slot)
of the last ten seconds of timestamp-playout frames, recomputed ten times per
second. It rises with sustained decode/network load, cannot be inflated by rare
stalls, and falls within the window, after which the normal hold/release
drains the buffer. Replay: `232347-186` >2 ms jerk 81 -> 44 per mille for
+1.0 ms median decode-to-submission; its startup connection 86 -> 78 for
+0.25 ms; the 19-minute `20260922-224404-796` session 372 -> 366 for +0.17 ms,
draining to 8.3-10 ms in clean 116 FPS stretches; Smooth `221707-518` neutral.
Rejected alternative: scoring each interval individually. At 0.5-1 ms
tolerance it pinned Balanced and Smooth at their caps for whole sessions (the
two-minute score remembers bad periods and late frames keep renewing the
hold); at 2 ms it drained below today's policy. Smooth and Low Latency are
unchanged. `tests/vrr/configs/latency-presets-stress.json` is a `vrrqueuesim`
config (it takes a decoded trace CSV); run through `vrrreplay` every assertion
reads null because those metrics are queue-simulator outputs.

Balanced p90 floor (2026-09-26): a median cannot see a tail. On lossy 900 Mbps
PyroWave (`20260926-131302`, 1440p and 4K, after the on-time release) 17-30% of
frames per ten seconds finished decode after their slot while the median floor
and the one-second mean (p50 254 us against the 500 us tolerance) stayed quiet,
so the buffer released 8.2 -> 4.6 ms with 10 growth decisions in 2.5 minutes.
Production Balanced then used `playout_readiness_floor_per_mille=900` (removed
later that day; see Profile consistency below). Replay
(exploratory; the captures fail the strict gate): >2 ms presented jerk 1440p
106 -> 54, 4K 88 -> 54, `232347-186` 44 -> 21 per mille, for +1.1, +0.6 and
+2.6 ms median decode-to-submission. p95 bought 46/47/21 for +1.4/+0.9/+3.4 ms.
`responsive-buffer-stress.json` passes on both new captures (delay max 10.4 ms,
p99 13.7 ms, zero modelled interval violations) and `latency-presets-stress.json`
passes in `vrrqueuesim`.

Historical profile consistency (2026-09-26, over `41312909`; superseded by
the customizable settings below): the latency profiles are one
dial. Low Latency, Balanced Target and Smooth differ in their on-time target,
source-frame allowance, hold and release, with each trade ordered the same
way; all wait in the same four-frame queue.
Their total playout-delay allowances are one, two, and four fitted source
frames, respectively. The former fixed 16 ms / 24 ms ceilings made the latter
two profiles fall below their frame allowance at slower source rates; those
fixed ceilings are removed. The worker queue's capacity remains a separate
safety bound, so the learned delay may still be lower than the profile's
allowance.
`testProfilesOrderEveryTrade` asserts the ordering. Low Latency's release rose
from 125 to Balanced's 250 us/s so it no longer drains slower than Balanced.
No profile uses the readiness floor above any more; the parameters remain so
captures that recorded them replay exactly.

The floor was removed after live capture `20260926-181149-830188` (4K 4:2:0
10-bit PyroWave, 485-680 Mbps, 116 FPS on the Deck). Both Balanced connections
(2, 4) and the Smooth one (5) began with the same ~1 s decoder backlog.
Balanced's floor counted its 50-128 ms ready offsets and reached the 16 ms
ceiling within 0.5 s; the three-frame queue then allowed only 16.86 ms, and the
decoder never recovered (decode call 7-9 ms, decode-unit queue p50 16-39 ms,
client timing 77-91%, 140-250 pacer drops, 326 frames cut at the on-time
deadline). Smooth's decoder recovered within a second (0.8 ms calls) and held
13.8 ms with 99.39% client timing and 33 drops. Replaying the Balanced
connections under all three profiles gave Low Latency 8.6 ms, Smooth 8.7 ms and
Balanced 16.0 ms on identical input. Excluding overloaded frames from the
floor (local decode over a source period, or ready offsets beyond the ceiling)
was tried and dropped: the first still lost the floor's help on brief stalls,
the second left the overloaded connections at 14.9-15.1 ms.

Replay over the 16 Deck captures from 2026-09-26, current policy against the
previous one: Balanced mean >2 ms jerk 153.0 -> 171.7 per mille, median
decode-to-submission 11.79 -> 10.49 ms; the overloaded connections drop from
16.0 ms to 8.4-8.6 ms, like the other profiles; Low Latency >= Balanced >=
Smooth in jerk on all 16 (previously Balanced beat Smooth on two). Balanced
loses the most where the floor had been holding it high: `000845` (9 -> 88),
connection 3 (4K 4:4:4, brief decoder stalls, 69 -> 141), and Smooth's
connection 5 replayed as Balanced (29 -> 74, its 29 came from the startup
backlog holding the floor up). Replay cannot show whether the pinned delay
kept the live decoder behind; one candidate is the PyroWave surface hold,
submitted on libplacebo's graphics queue behind renders prepared ahead into
mailbox images. Exact baselines pass for `181149` connections 2 and 4 and
`155747`; `000845` connection 4 and `181149` connection 5 are inexact on the
unmodified baseline as well (8 and 1 frames with targets but no submission).

The pacing worker's on-time reassembly lead leaves out frames whose own
decode (reassembly to decode complete, less a deliberate hold) took longer
than a source period: releasing later frames earlier cannot relieve an
overloaded decoder, and counting them moved every deadline before the frames
finished arriving.

Decode hold clear of the Present (2026-09-26): the full sandbox capture
`Moonlight-sandbox-20260926-133637-302` (4K Balanced, 890M, PresentMon joined
by QPC) showed Present calls with 75 per mille >2 ms jerk but 161 per mille on
screen. On-screen time equals PresentMon render-complete time on every frame;
Moonlight's render fence was always ready before the call. Flips landed
1.10 ms (p99 1.95) after Present when the next frame's PyroWave decode was
submitted after the Present call returned, but 2.6 ms (p90 3.9) when that
decode was already running (11% of frames). Attribution of on-screen jumps:
decode on the GPU at Present 75, late arrival 41, latched presents 40 per mille.
The pacing worker now publishes a window per frame from its target minus this
machine's learned p95 decode GPU time (`decode_complete - decode_submit`) and
closes it when its Present call returns; a PyroWave decode submitted inside it
waits for the close (yield loop), only if the frame's reassembly deadline is
still met at target + learned p95 Present-call duration, never beyond that
deadline or 4 ms. Nothing in it is a GPU-specific constant: the benefit scales
with how much a GPU delays flips behind compute. The hold is excluded from the
controller's decoder-queue term, the overlay decode statistics and the receive
deadline's learned cost; the trace records it as `decode_hold_us` and replay
applies it. Replay cannot model GPU contention; a live PresentMon capture must
confirm it.

Latched presents on the 890M were also slow: `Present(1,0)` ran as
"Hardware Composed: Independent Flip" and reached the screen 4.5 ms (p50) /
16 ms (p90) after the call versus 1.1 ms for tearing presents, and one latch
pushes the flip anchor so the next frame latches too (603 of 733 controller
latches came 8.4-20 ms after the previous target). A latch-only-after-floor-gap
policy was tried and rejected for now: replay jerk 66 -> 59 per mille, but the
controller tests showed post-stall catch-up bursts serializing on the software
floor (drops, windowed-smoothing latency bounds) where the flip queue absorbed
them, and the slowness was measured on one driver. A general version would
first measure latched flip lateness per machine (DXGI's latched flip times are
reliable) and switch only where latching is slow.

Synchronized DXGI flips (2026-09-28, opt-in): `MOONLIGHT_VRR_SYNC_FLIPS=1`
makes D3D11 present every VRR frame with `Present(1, 0)`, as Linux's
Mailbox/FIFO presentation never tears. It was briefly the default and was
reverted to opt-in the same day. Three later 4K 4:4:4 Plague Tale sessions
(`20260929-002816-302`, `-003251-756`, `-003353-351`) had these median
Present-to-screen times: 6.4 ms, 9.8 ms and 1.1 ms. In the two slow ones,
blocked Present calls pushed submission jerk above 2 ms to 363-487 per mille
and on-screen jerk to 101-325. One of them had 1,130 consecutive-frame pairs
four refreshes apart, consistent with a slower composited flip path. Without
the variable, the per-frame tearing path and flip protection below apply.
The controller still plans latched and adaptive slots.
An adaptive slot the presenter synchronizes is reported as
`flip_protection_latched` with no reference time, so the controller anchors it
as a latch and replay applies it as recorded evidence. Replay's DXGI argument
contract and exact baseline therefore hold unchanged. The motivating evidence
was DXGI refresh counts (`latch_sync_refresh_seq` per `latch_submission_id`)
in captures `20260928-225055-539` and `20260928-230651-821`. There, every
present that shared a refresh with its predecessor followed a tearing present.
That evidence is weak: DXGI credits a tearing flip to an earlier refresh
(recorded tearing flip times had a median 8 ms before the Present call), so
some of those shared refreshes may be accounting rather than lost frames. The
raster guard now resets per swap chain, instead of staying disabled for the
session after three timeouts during the fullscreen transition.

Raster flip guard (2026-09-26): after the decode hold, PresentMon on
`Moonlight-sandbox-20260926-140622-380` (4K Balanced) showed on-screen jerk
>2 ms at 59 per mille (from 161) and 57 tear candidates in ~290 s (on-screen
interval below the panel's 8.15 ms latched minimum, excluding latched pairs):
33 tearing->tearing pairs whose calls were 9.75 ms apart but whose first flip
landed late (3.4 ms p50, up to 12 ms), and 24 tearing->latched pairs. The
SyncQPCTime check missed them because a tearing flip's refresh time is
credited to an earlier refresh. `D3DKMTGetScanLine` does follow VRR on this
panel (aligned capture 20260901-192940: 1% in vblank 8-9 ms after the previous
Present, 84-99% at 9-16 ms, ~50% beyond 17 ms where the panel re-scans at its
floor). Native flip protection now always opens the raster source and, before
a tearing present, polls frame statistics and the raster until nothing is
pending and the panel is in vertical blank, then presents with tearing
allowed. After two display periods it latches, and three consecutive timeouts
disable the raster wait for the session (logged) in favour of the old
frame-statistics check, which also remains the fallback when the raster
cannot be read. The wait is inside the presenter, so recorded submission
times include it and replay stays exact. Unverified live.

Native flip protection (2026-09-23), based on `5c5ba95b`: the flip anchor
assumes a tearing present flips at its call, but on the Radeon 890M DXGI took
p50 ~3.2 ms / p95 ~6.7 ms. A latched successor therefore flipped 2-8 ms later
than anchored, and in capture `20260922-224404-796` 68% of adaptive presents
following a latched frame were issued inside its scanout (its SyncQPCTime
refresh start, recorded in the latch/frame-stats fields). Production now sets
`native_flip_protection=1`. Immediately before an unlatched DXGI Present the
worker passes one display period as `VrrPresentRequest::flipProtectionWindowUs`;
D3D11 queries `GetFrameStatistics` (~6 us) and presents `Present(1, 0)` instead
when the predecessor is not yet displayed or the last refresh began less than
one period earlier. The controller keeps its planned latch decision and
hysteresis; `noteNativeFlipProtection()` only advances the flip anchor from
the observed refresh. Frames that were safe keep their adaptive flip; a latch
on a panel already idle in its VRR blank flips at once. Trace rows append
`flip_protection_*` columns (still schema 5); replay applies the recorded
latch as execution evidence rather than simulating DXGI, so older captures
reproduce unchanged (verified on `224404-796`: all submissions, targets, tear
classes and controller state exact, as before). Vulkan and composition
presenters ignore the request. Needs a live capture to confirm tear removal.

Reduce judder follow-up (2026-09-22), based on `e053b5cb`: the smoother's
positive retiming cap rises from 2 ms to 6 ms, a learned readiness reserve
delays the smoothed schedule by the lateness the smoother itself causes, and
phase-error feedback lets its period follow drifting game rates. All three are
new zero-default controller parameters, so older captures replay unchanged.
See the section below and section 8.3.

Windows high-bitrate follow-up (2026-09-22), based on `26675aa8`: D3D11 VRR
presentation no longer holds FFmpeg's decode lock on separate devices, and a
monitored decode fence now supplies the decode-completion observation that
production source mapping assumes. See the section below and section 10.1.

Controller-feedback update (2026-09-19), checked against `9362b0f0` and its
common-library waveform protocol: section 12 now covers Windows
Bluetooth waveform output and the shared adaptive-trigger path. This update
does not change video timing or replay policy.

Reference baseline: `06fae71f` (vrr17 branch), plus the client-warning and
gradual backlog-recovery follow-up described below. This includes source ownership,
buffer attribution and decode-wait starvation prevention (2026-09-20).
The Windows buffer-retention follow-up is based on `e1df34b7` (2026-09-21).
The 2026-09-21 policy recorded `playout_recent_pressure_release=2`: only a fresh,
readiness-attributed interval error with absorbable service renews the existing
clean-time hold. Submission jitter after readiness and sustained service
overload still lower the timing score, but cannot indefinitely retain previously
acquired buffer. The preset hold durations, release rates, growth law and caps
are unchanged. Revisions 0/1 retain their historical replay behavior. This is a
shared-policy correction, not evidence that Windows GPU execution became faster.
The current policy uses revision 3: it preserves earned clean recovery across
short sequence breaks while requiring fresh sequence qualification before
release. Long unobserved gaps do not provide recovery time.
The selected Windows capture ends with zero attributed readiness lateness yet
revision 1 renews its full eight-second hold. Its recorded submissions and
controller diagnostics reproduce. The 2026-09-22 replay audit now recognizes
the D3D11 fence poll recorded at the final Present boundary, after preparation,
while retaining support for older preparation-poll traces. With that audit fix,
the selected `20260922-184032-168` capture passes exact controller/worker
replay. Its display model remains uncalibrated and cannot prove optical tearing
or tear freedom. Live improvement from the buffer change still requires a fresh
test.
The 2026-09-21 preparation-stage follow-up is based on `18602b1c`, including
Gemini's decode-completion source mapping and preparation-on-arrival changes.
Linux VAAPI/Mailbox has experimental offscreen preparation independently of the
pacing thread, as described in section 7.2. Following live 4K throughput
regressions, this requires `MOONLIGHT_VRR_OFFSCREEN_PREPARATION=1`; the default
retains the direct asynchronous hardware-source path. Other backends retain their
existing execution path. This changes execution overlap, not buffer ceilings
or source cadence policy; physical smoothness still requires a live retest.
Production now selects serial-service revision 2: the shared interval buffer
compares workload with intended time over its qualified one-second window,
rather than treating one slow frame as sustained overload. Deferred D3D GPU
service counts residual CPU waiting, not intentional pacing hold; a fence
verified pending at the final wait supplies readiness lateness. Historical
revisions 0/1 remain available. See
[service-gate correction](docs/vrr-service-gate-correction.md).
Live GPU diagnostics now add a separate asynchronous CSV under existing deep
tracing: CPU dependency
spans, source-retirement bounds, output readiness before presentation, and
libplacebo's delayed shader-duration history. Shader samples are not tagged to
their originating frame and do not expose absolute GPU start times. See
[live GPU tracing](docs/gpu-live-tracing.md). That tracing does not alter replay
schema or policy; the service-gate correction above changes production policy.
GPU diagnostic revision 2 removes the decoder-thread surface-status query: live
revision-1 captures showed it blocking behind another frame's decode synchronization.
Revision 2 retained the worker-side query for retesting.
Revision 3 timestamps existing packet send/receive, packet delivery/assembly and
pacer handoff, associates output surfaces with frame IDs, and samples Linux
thread CPU time/context switches around send/receive, VA sync/status and render
commands. Decoder-thread instrumentation only reads metadata and OS counters;
it performs no new driver calls. These spans expose CPU-versus-blocked time and
cross-thread overlap, not internal driver locks or GPU engine execution times.
Revision 4 removes the worker-side VA status query as well. The latest completed
4K HEVC capture `20260921-005750-93776` recorded 1.05 ms mean and 7.62 ms p95
inside that diagnostic call, before the required `vaSyncSurface` wait. Timing
the existing synchronization preserves readiness and its measurements without
adding this potentially blocking probe. This removes diagnostic driver work;
some of its wait may move into synchronization, so FPS recovery is not implied.
The current follow-up enables early preparation on both platforms and
asynchronous VAAPI/Vulkan Mailbox output, described below. That follow-up still needs
live validation; the baseline's latest high-bitrate run delivers about 100 FPS
at a 120 FPS source despite the starvation improvement. The
earlier timing lineage remains `1ccefb6e` (vrr17.1), plus the buffer-accounting review and
initial-calibration follow-up (2026-09-18). Accounting adds separate buffer
reasons, latency breakdowns and replay audits. The follow-up restores vrr14's
slot-only presentation-protection threshold, expands the preset allowances to
2/2/4 source frames, and shortens initial qualification without accelerating
ongoing buffer growth. See [VRR17 review](docs/vrr17-review.md) for the historical
comparison and [calibration follow-up](docs/vrr17-calibration.md) for current
policy, tests and validation limits. The existing D3D11 4K binding eligibility change
(2026-09-17) means single-device streams at least 3840x2160 bind instead of copying
when Feature Level 11.1+ or D3D11 fences are available. Existing Intel and
separate-device binding, lower-resolution copy behavior and explicit overrides
are unchanged. That binding-only change left backbuffer clearing, context
locking, pacing, buffering, native presentation and replay alone. Its subsequent
raster-guard, buffer-first and startup/replay experiments have been removed at
the user's request after continued tearing; their changes are archived outside
the worktree. This binding-only restoration is not yet live-confirmed tear-free.

The follow-up [user-facing diagnostics](docs/vrr-diagnostics.md) adds a shared
Windows/Linux **Trace VRR frames for debugging** checkbox and local ZIP export.
Recordings are saved in per-stream subfolders of `vrr-diagnostics` on the user's
Desktop. Tracing does not select or modify timing policies, and Moonlight does
not upload the logs. The unpublished timing-comparison selector was removed.

The baseline includes Linux VRR probe/playback color-range alignment
(2026-09-16), originally `352f4827` plus removal of the Allow tearing preference
(2026-09-15),
client-processing,
vrr14-style compact stats reporting, restored Reduce judder, reconnect
trace preservation, motion cadence telemetry, hard buffer ceiling, AMD low-latency decode request, observed-latency trace diagnostics, and removal of the latency oscillation test,
inspected 2026-09-15; now includes responsive readiness revision 4, desktop-rate isolation,
fence-value-verified Windows readiness waits, bounded Vulkan source retirement,
bounded GPU-readiness head-start adaptation, and cadence-gated,
elapsed-time source-offset recovery. The latency
presets and persistent Vulkan presentation changes remain active.
Windows, Linux, and macOS share one production queue policy: absolute client-added
interval error drives a severity-weighted quality score. The boxed VRR timing
settings expose four independent values. Presets only fill in those values:

| Preset | Source-frame allowance | Quality target | History | Interval tolerance |
| --- | --- | --- | --- | --- |
| Low Latency | 0.5 | 99% | 60 seconds | 0.5 ms |
| Balanced | 1 | 99.5% | 120 seconds | 0.5 ms |
| Smooth | 4 | 99.99% | 300 seconds | 0.25 ms |

Custom bounds are 0.25–4 source frames, 90–99.99%, 10–300 seconds, and
0.25–2 ms tolerance in 0.25 ms steps. Higher tolerance accepts more interval
variation before counting a quality miss; it does not relax native submission
spacing or scanout safety. The score is a controller goal, not a guarantee of
physical display smoothness. All settings share an eight-second clean hold,
250 us/s release and all-rate admission policy. Old mode-dependent hold/release
and the implicit 0.2 ms Smooth tolerance remain only in historical parameter
snapshots. The queue still has four waiting slots; its capacity independently
limits delay, so an allowance is a ceiling rather than a fixed delay.

`VrrTimingOptions` defines defaults and bounds. Missing saved values migrate
from the saved preset; invalid persisted values are bounded before use. The
session snapshots all four values and carries them through decoder recreation
to the pacer. Calibration identity uses actual values rather than the last
preset name. The first three map to existing trace parameters; the new
`playout_interval_tolerance_us` records tolerance explicitly. Its zero default
preserves historical tolerance selection for old traces. Session-policy replay
restores all four recorded values for customizable captures. Exact replay always
uses the full captured parameter snapshot.

The timing controls support gamepad Tab/Shift-Tab focus navigation and left/right
adjustment, including when a numeric text field has focus. The editable VRR
fields route the editor's left/right key signals directly to the adjustment
handlers before text editing (input correction reviewed against `ff6f071c`,
2026-10-08). Forwarding key events to the parent SpinBox alone lets its editor
consume presses as cursor movement. `tests/gui/tst_vrrtiminginput.qml` covers
single presses, repeated adjustments and bounds on all four fields.
Preset and PyroWave
calibration-host popups use `AutoResizingComboBox`, switching the gamepad to
arrow/Return navigation while open and restoring UI navigation on close.
The calibration host resolves its target-card focus on each closed-menu Tab
event, leaving popup arrows to Qt. Its former unconditional Down handler
stole focus from the open menu. Left/right shortcuts change a combo selection
only while its popup is closed; open-menu confirmation and cancellation retain
Qt's native behavior. A plain opaque popup background also keeps the list
readable with Qt Quick software rendering.
The SDL dispatcher retains each controller button's key and modifiers until
release, even if opening/closing a popup changes navigation mode during the
press. Otherwise Return-down can become Space-up and reopen a confirmed menu.
Pending mappings are cleared on disable and when stale events are flushed.
Reconnect after changing timing values.

Initial interval calibration requires at least 500 ms of contiguous coverage
and 32 valid intervals. Ordinary growth remains at most 250 us per 250 ms,
applied at most 125 us per frame. Once qualified, a sequence break requires the
historical one-second requalification; FPS changes cannot rearm fast startup.
Live sessions also cap the preset allowance against the fitted source period,
not only the negotiated stream rate. Successful Windows present-ready fence
waits and synchronous Linux Vulkan completion polls can feed a
separate bounded readiness lead: a recent p99 wait plus 500 us, clamped to 12 ms
and one source period. Linux VAAPI retains source mappings until their GPU reads
finish and uses the Mailbox swapchain's GPU semaphore for output completion.
Other presentation modes, imports and software frames retain the bounded CPU output poll. A learned
lead advances only the render-start deadline; it does not move the source
presentation target or claim that the GPU will complete on time.
Explicit captured parameters keep the new controls
disabled unless the trace records them, preserving exact replay of older captures.
The minimum remains 1 ms (subject to capacity). Five-minute version-20 raw
readiness calibration is diagnostic only and cannot inflate the live request.
Linux's event-gated history-version-19
policy is retired from live selection; explicit historical parameters remain
supported for exact replay. Linux calibration keys are segregated from that
retired policy. Native synchronization and presentation remain backend-specific.
Display feedback is optional diagnostic evidence and does not steer this policy.
The former V2 Queue A/B checkbox and its runtime configuration fields are removed.
The saved `v2queue` setting is ignored and removed on save, so previously disabled
installations also use the new queue. Legacy policies remain only for explicit
historical diagnostic configurations. Reconnect after changing latency presets.
Updated 2026-09-18: displayed frame queue delay still excludes the worker's
explicit GPU decode synchronization wait. Existing decoding, queue and rendering
statistics keep their definitions. Advanced tracing shows the wait on its own
labelled line; it is never merged into an old statistic. Full decoder-output-to-Present-
return time remains diagnostic, with no new aggregate overlay headline. Queue
plus rendering plus that separate wait partitions internal client processing.
The initial map came from nine Luna Medium specialists, followed by
targeted source checks and corrections. No live capture, optical measurement,
build, or test run was part of this documentation investigation. Recheck the
named functions after changes; comments, diagnostic labels, and old experiments
can disagree with the active implementation. The historical D3D11 latch mismatch described below
is one concrete example; the current native boundary now forwards the selected interval.
Historical 2026-09-08 builds selected `playout_adaptive_only=1`. The current
production resolver uses `playout_adaptive_only=0` and per-frame latch requests;
source-rate/presentation safety decisions can therefore select a latched frame.
Explicit captured parameters preserve both behaviors in replay.
Updated on 2026-09-10: the forced-composition and per-frame repaint checkboxes
have been retired. Their saved settings (and the older Mailbox preference) are
ignored and removed on settings save. Session startup no longer invokes the
composition guard and passes repaint=false to the decoder. Dormant renderer
helpers and their deterministic tests remain available for development.
Production retains its Immediate/WSI FIFO selection; adaptive presentation
permission is owned by the VRR backend rather than a user preference.

### Windows PyroWave parity (2026-09-28)

Windows D3D11 PyroWave frames, whose Vulkan decode signals a shared fence and
returns at submission, are now marked `decoderOutputComplete=false` like Linux
shared-surface frames.
- Decode-cost learning (`observeGpuCompletion`) therefore uses only waits that
  `waitForPyroWaveDecode()` actually observed, never the CPU submission time.
- The decode-hold bound and the reassembly deadline depend on that learned cost.
- Both platforms keep four PyroWave frame contexts in flight.

Deferred swapchain acquisition stays Linux-only. The flip coupling it removes
comes from a RADV Mailbox image held across the target wait; DXGI flip-model
presentation has no equivalent acquired image.

### Cached starting delay (2026-09-27)

A session now starts at the playout delay that the last session with the same
calibration key settled at, instead of the generic start of 0.95 of a source
period clamped to the display period. The key covers host context, stream
format, display, rates, frame smoothing and latency mode.
- **Recording:** `VrrPacingWorker::noteSettledDelay()` samples the decision's
  playout delay once per second after the first 30 s.
- **Saving:** on stop, a session with at least 60 samples saves their median to
  `vrr-start-delay.json`, beside the calibration cache. `Vrr13::saveStartDelay()`
  keeps 16 entries with a 14-day expiry.
- **Seeding:** `VrrPacingWorker::start()` loads it into
  `playout_delay_start_seed_us` through
  `VrrTimingController::seedPlayoutDelayStart()`. `playoutDelayStartUs()` then
  returns the seed clamped to the policy minimum and maximum.
- **Replay:** the seed is a recorded parameter, and session-policy replay
  copies it from the capture, so replay starts where the live session did.
  Captures without it are unchanged.
- **Limits:** the seed sets the start only; growth, holds and release behave as
  before. A session whose early arrivals are steadier than its later ones
  still starts above its early need.

### Decode-hold bound and deferred Vulkan acquisition (2026-09-27)

The PyroWave decode hold is now bounded by `g_HoldAnchor`.
- `VrrPacingWorker::publishReceiveDeadline()` publishes it as the frame's
  target minus the recent p95 reassembly-to-decode duration, the p95 worker
  preparation (`m_PreparationCost`, which includes the output-completion wait)
  and 250 us.
- The reassembly deadline, `g_Anchor`, still omits preparation and only governs
  partial-frame release.
- Previously, a hold ended at the reassembly deadline. In capture
  20260927-215902, 289 held frames were late: every one finished decoding
  before its target, but its 5.8 ms median preparation did not fit afterwards.
  Half the interval-buffer growth events came from such frames.

Linux Mailbox VRR always uses deferred swapchain acquisition:
  - `PlVkRenderer::prepareFrame()` renders into one of two renderer-owned
    textures, using the last swapchain frame's parameters.
  - `presentAdaptive()` acquires the swapchain image, blits the texture into it
    and submits.
  - No swapchain image is held across the target wait, but each frame pays one
    full-screen blit, and the present call now includes acquisition.
  - A size, format or window-state mismatch falls back to direct acquisition.

Live 4K 4:4:4 Deck sessions halved Present-to-flip (3.9 to 2.0 ms median at
Low Latency, 1.4 ms at Balanced) with no throughput loss. The Present call rose
from 0.15 to 0.32 ms median. Gamescope (Immediate/FIFO) and D3D11 are unchanged.

### Source-epoch buffer restore and startup render sample (2026-09-27)

Three session-policy controls were added. Captured traces without them replay
unchanged.

- `playout_epoch_rate_ratio_per_mille` (1500), `playout_epoch_confirm_us`
  (1 s) and `playout_epoch_sustain_us` (10 s) apply to the interval buffer:
  - When the fitted source rate departs from the current epoch's rate by at
    least 1.5x and holds for the confirmation time,
    `noteIntervalBufferEpoch()` records the outgoing demand. It does this only
    if that epoch lasted the sustain time, keyed by quarter-octave rate bucket.
  - On entering a bucket, or its neighbour, with a lower recorded demand, it
    restores that value through `IntervalBuffer::restoreTarget()`. The restored
    target is then protected for one hold.
  - Restores only lower the target; an unfamiliar rate keeps current protection.
  - This removes delay bought during a slow scene, which Smooth's
    50 us-per-second release would otherwise carry for minutes after the source
    returned to its earlier rate.
- `playout_delay_decrease_slew_us` (250) limits each applied decrease to
  250 us per frame.
- `preparation_initial_sample_excluded` (1) keeps the first preparation after a
  controller start or phase rebase out of `m_PreparationDurations`. That
  preparation includes one-time renderer setup (0.4 s on the Deck at 4K). As the
  only sample, it set `typicalRenderUs()` to its 100 ms clamp and delayed the
  following targets.

The stale-drop horizon, `max(2 periods, playout delay + 1 period)`, now protects
the playout delay in every session rather than only under release revision 3.

Replay controller overrides now accept `"base": "session"` per scenario, or
`--session-base` on the command line. The overrides are layered on the session
policy resolved for the capture. Without a base, overrides still start from
historical defaults.

Exploratory replay of the 15-capture corpus, which cannot model frame shedding:
- The 2026-09-27 30 fps scene capture's decode-to-submission p95 went from
  23.8 to 19.0 ms, with presented jerk over 2 ms +1 per mille.
- Two Balanced 2026-09-26 captures gained 0.3-0.7 ms p50 at +3 to +4 per mille.
- The remaining captures were unchanged.

### Reduce judder readiness bound and buffer attribution (2026-09-30)

Production interval-buffer sessions with Reduce judder enable
`playout_smoothing_readiness_bound=1`. Its zero schema default preserves the
old smoother and buffer attribution for captures that lack the field.

Early retiming may use only the current frame's known decode-readiness slack:
the total smoothing adjustment cannot fall below
`min(0, readyOffset - delayBeforeThisFrame)`. This retains the raw target's
typical-render allowance. The existing execution clamp still handles worker
backlog and later preparation variance. The smoother's next clock basis and
its readiness-reserve observations use the requested, unconstrained retiming;
feeding a readiness-clipped target back into that clock propagates a single
late frame and spoils ordinary host-quantized cadence.

The interval observer's readiness deadline is the later of the raw and applied
smoothed targets. A frame ready before its raw deadline can therefore neither
grow the standing playout buffer nor renew its clean-time hold merely because
Reduce judder attempted an earlier slot. Genuine raw-readiness misses still
grow protection under the existing interval-quality, capacity and service gates.
Smoothing's own reserve remains separately bounded to 3 ms. A lower source rate
supplies processing/spacing capacity; it does not guarantee that every frame is
ready a fixed number of milliseconds before its timestamp-playout deadline.

The deterministic 120-to-30 FPS fixture learns the heavier 9 ms preparation cost
before adding alternating 21/45 ms sender intervals and 4 ms readiness variation.
With release frozen to isolate growth, all noisy frames are ready before their
raw targets. Smooth previously made 60 smoothing-only growth decisions, raising
8 ms to 9.278 ms; the corrected policy keeps 8 ms with zero growth. With normal
release enabled, delay drains to 5.789 ms instead of remaining at 9.274 ms;
genuine raw misses as delay drains still retain protection. Separate
late-readiness variants retain growth in all three presets. This is controller
evidence, not a measured live improvement.
The newest completed local capture, `20260930-213542-1353491`, stays near 95 FPS
and does not contain the reported slowdown. Its controller decisions reproduce,
but the full exact replay gate fails; it cannot establish strict live A/B results.
The smoothing calibration identity includes this bound to avoid seeding the new
policy from a delay acquired under the old attribution.

### Reduce judder readiness reserve and wider retiming (2026-09-22)

Written on the Sunshine host (Ambidex), which has no client toolchain, traces or
share access. Evidence is a synthetic harness around the real controller
(g++ with FFmpeg/SDL stubs) plus new deterministic tests; no capture replay,
application build or live test accompanied it.

Two defects limited what Reduce judder could correct:

1. **Readiness clamps undid the smoothing.** Centering a smoothed schedule on
   uneven stamps moves late-stamped frames earlier than their raw slot. Because
   arrival follows the stamp, those frames are frequently not ready: the target
   clamp presents them late and restores the step. The negative bound was the
   whole playout delay, i.e. down to the mapped source slot itself. The
   interval-quality buffer does not respond, because 1-2 ms errors on 5-17% of
   frames average under its 0.5 ms tolerance. The 2026-09-10 SteamOS capture
   showed the same thing live: 0.4% of intended pairs over 2 ms jerk, 13.8%
   after readiness clamps.
2. **The 2 ms positive cap was too small** for host-refresh quantization. A
   game at 90 FPS captured from a fixed 120 Hz host arrives as 8.3/8.3/16.7 ms
   stamps, needing roughly ±2.8 ms of retiming; 100 or 110 FPS need more. The
   cap also clipped only one side, pulling the schedule early.

A third limitation affected drifting rates: the period followed a 2.5% interval
EMA only, so a game ramping between 70 and 100 FPS left the smoothed slot
several milliseconds from its stamps (≈2.7 ms mean in the new drift fixture).

Changes, all active only with Reduce judder enabled:

- `playout_smoothing_max_lag_us` 2000 → 6000.
- `playout_smoothing_reserve_*` (max 3000 us, p980, tolerance 500 us, release
  500 us/s): for each frame the smoother placed, the controller records
  `min(readyOffset - playoutDelay, 0) - retiming`, the lateness caused by moving
  the frame before its raw slot. Delivery that misses the raw slot remains
  playout-buffer evidence. Worker backlog is excluded because it follows the
  previous, already-reserved target and would feed the reserve back into
  itself. The reserve is the p98 of the last 128 such values minus the
  tolerance, acquired at most 250 us per frame after 32 samples, and it is
  applied to every timestamp-playout frame while smoothing is enabled, so
  cadence resets do not step by the reserve. It shares the 6 ms positive
  retiming budget and is reported inside `cadence_smoothing_us`, never
  `playout_delay_us`. The controller exposes it as `smoothingReserveUs()`;
  there is no dedicated trace column.
- `playout_smoothing_period_feedback_per_million=20000`: the smoothed period
  also integrates 2% of each frame's phase error (a second-order tracking loop,
  damping about 0.5 with the 15% phase gain).

Synthetic results (60 s, three seeds, host-present stamps plus delivery jitter
of 0.35 ms mean with 1% 2-5 ms spikes; per mille of pairs over 2 ms jerk,
mean decode-to-submission change against Reduce judder off). The harness lets
Balanced release to its 1 ms floor, so its row is a small-buffer worst case:

| Content | Balanced previous → new | Low Latency previous → new | Smooth previous → new |
| --- | --- | --- | --- |
| 90 FPS on a 120 Hz host | 442 → 50‰, +0.34 → +1.31 ms | 194 → 28‰, -0.25 → +0.42 ms | 47 → 2‰, -0.53 → +0.00 ms |
| 70 FPS on a 120 Hz host | 458 → 39‰, -0.23 → +1.39 ms | 231 → 23‰, -0.64 → +0.45 ms | 71 → 2‰, -0.86 → +0.00 ms |
| 70-100 FPS ramp on a 120 Hz host | 562 → 86‰, +0.11 → +1.54 ms | 370 → 44‰, -0.59 → +0.63 ms | 201 → 8‰, -0.56 → +0.03 ms |
| Paced 90 FPS, 2 ms stamp jitter | 80 → 25‰, -0.37 → +0.08 ms | 70 → 21‰, -0.40 → +0.03 ms | 67 → 20‰, -0.42 → +0.01 ms |
| Even 120 FPS | 18 → 17‰, +0.00 → +0.00 ms | 5 → 5‰, 0 → 0 ms | 0 → 0‰, 0 → 0 ms |

The previous policy's negative latency deltas are the early bias the one-sided
cap introduced, not free smoothing. Random-walk frame pacing (a game whose own
frame times vary by 2-3 ms) improves less and costs more (Balanced 60 FPS,
3 ms: 352 → 182‰ for +1.9 ms), because no smooth line stays close to it.

The new deterministic fixtures (`testReduceJudder*`) cover 90 FPS on a 120 Hz
host with production and tight buffers (tight: 68.1% → 1.0% of pairs over 2 ms,
+0.9 ms), even stamps with delivery spikes (no reserve may be acquired), reserve
release once pacing becomes even, and a 70-100 FPS ramp (standing retiming
offset about 2.7 → 0.56 ms). All existing controller fixtures pass unchanged
except the assertions that named the old 2 ms cap. The replay-config round trip
test was added but not compiled here (it needs Qt).

**Cadence-reset easing (2026-09-22 evening, ALLYTWO).** Capture
`20260922-193211-380` (116 FPS on the 120 Hz LG, game at 112-116 FPS, host
stamps on a ~2.15 ms grid) showed that a four-interval slowdown of the game
(10.7-12.9 ms frames) fails the windowed stability gate, and the reset dropped
the accumulated retiming (often -5 ms) to zero in one frame, so a 10.7 ms host
interval was presented as 13.8-15.9 ms. The smoothing value logged in
`cadence_smoothing_us` includes the reserve, so resets never read as zero there;
identify them by a step in retiming, not by a zero value.
`playout_smoothing_reset_slew_us` (production 1000 with Reduce judder, 0
otherwise and in older captures) now eases the previous frame's retiming toward
the raw slot by at most that much per frame when the smoother resets, within the
same positive cap and negative `-(playout delay + reserve)` bound. A new clock
epoch (`rebased`) and a phase reseed still jump. The eased slot becomes the next
smoothed basis, so a re-engaged smoother continues from it. Replay of 193211
with a display model (latched presents queue behind the previous flip, adaptive
presents flip at call + 1.28 ms): steady-state distinct snaps (displayed jerk
over 4 ms) 36 → 27 in 59 s, client-stretched snaps 10 → 4, latency unchanged,
0 modelled tears. The remaining 21 are host intervals of 12-21 ms passed through.
The earlier 97 FPS capture `184032-168` is neutral (71 → 73). Fixture
`testReduceJudderEasesCadenceResets`: >2 ms pairs 2.7% → 2.0%. Not a
calibration-key change. Also from that capture: the first 7 s had 205 GPU
fence waits over 3 ms (none afterwards) where the CPU decode-completion wait
was short and rendering queued behind decode, a startup transient.

Still required on ALLYTWO: the Qt suites, exact replay of the newest capture
(captured parameters lack the new fields, so it should still reproduce), a
`configs/judder-reserve-variants.json` batch on real captures, and a live
comparison. The synthetic harness does not model GPU render variance, the
decode wait, stale-frame dropping or native presentation.

Fixed in the current source: with a 120 FPS stream, a sub-60 FPS game on a 60 Hz host
(16.7/33.3 ms stamps) previously failed to fit its source period. Each 33 ms interval is a
major cadence departure at the 8.3 ms negotiated period, and the following
16.7 ms interval counts as a return to stable cadence, clearing the cadence
window. The fitted period stayed at 8.3 ms, every long interval was a phase
discontinuity, and smoothing did not engage. Production now uses a 5:4
return-to-stable band while old captured parameters retain 2:1. A deterministic
60/30-pattern fixture accepts the 40 FPS average; live visual effect is unmeasured.

### Windows decode/presentation decoupling (2026-09-22)

Two Windows-only gaps against the Linux path were found from source, both
growing with bitrate. No live capture, build or replay accompanied the change;
this document was written on the host machine, which has no client toolchain.

1. **Shared decode lock.** FFmpeg's D3D11VA hwaccel holds the renderer-supplied
   lock for its whole per-frame submission (`DecoderBeginFrame` through
   `DecoderEndFrame`, including the bitstream copy). The VRR worker held that
   same mutex through preparation, the render-context `Flush`, `Present` and
   the DXGI statistics queries, and took it twice more around the present-ready
   wait. With a playout buffer near one source period, frame N's target lands
   near frame N+1's decode submission, so the collision recurs with the cadence
   and lengthens as larger frames make submission slower. Stock Moonlight, and
   this fork's legacy path, take the lock only for decode-context calls on
   separate devices. This is the Windows form of the Linux finding in
   [live GPU tracing](docs/gpu-live-tracing.md): decoder-thread driver work
   serialized behind the worker's wait on another frame.
   `D3D11VARenderer` now has a separate presentation mutex for the render
   context, swapchain and prepared-frame state, shared with window-change
   callbacks and legacy rendering. It includes FFmpeg's lock only when decode
   and render share one immediate context. `renderVideo()` takes FFmpeg's lock
   solely around its decode-context `Signal`/`Wait`. Lock order is presentation,
   then context; the decoder thread takes only the context lock.
2. **No decode-completion observation.** Production maps source time from
   `decodeCompleteUs` so hardware decode time is absorbed into the sender
   offset (`93363745`). Linux supplies it from `vaSyncSurface()`. `78b99f1c`
   had made the Windows decode check nonblocking, so `decodeCompleteUs`
   stayed equal to decoder output, which precedes the hardware decode. The
   whole decode duration then had to fit inside the capped playout buffer and
   the residual present-ready wait. `waitForDecode()` again waits for the exact
   captured decode-to-render fence value, without any context lock, using a
   dedicated event. The GPU-side `Wait` in `renderVideo()` remains the
   correctness mechanism. The CPU wait applies only to monitored fences; the CPU
   cannot observe non-monitored fences, and shared-device sessions capture no
   boundary, so both keep the previous nonblocking behavior. A failed or 50 ms
   timed-out wait disables adaptive presentation and requests recovery without
   advertising readiness. The last user-confirmed smooth Windows 4K capture
   (2026-09-11, below) ran with this style of blocking decode wait.

Expected trace differences: Windows rows gain nonzero `decode_sync_wait_us`,
as Linux rows have; `gpu_ready_wait_us` at the target should fall toward
zero; the lock-wait spans `gpu_ready_poll_start_us - present_start_us` and
`native_present_start_us - gpu_ready_time_us` should no longer track the next
frame's `decoder_output_us`. These changes do not alter controller parameters,
buffer caps or replay policy. They require a Windows build, the deterministic
suites, exact replay of a new capture and a matched high-bitrate live test.

### Client warnings and gradual backlog recovery (2026-09-20)

Production captures `playout_late_recovery=1` and
`playout_catchup_per_mille=20`, independently of Reduce judder. Recovery arms
when a successfully submitted frame exceeds its original target by more than
500 us, or replaceable queue age exceeds one source period. This also covers
preparation/submission stalls that happen after scheduling without queued
backlog. Failed/cancelled presentations do not arm recovery. Qualified source
intervals within 10 percent of the fitted period are required; cadence breaks,
rate transitions and unqualified timing bypass the floor.

A soft submission floor initially limits catch-up to a two-percent reduction
in source interval. Replaceable queue pressure progressively permits faster
recovery up to the display period plus guard. Intentional playout delay is
subtracted from that pressure. With no display headroom, the soft floor is
inactive and existing native protection still applies. Recovery never changes
the present mode or removes a latched request. Each additional hold is bounded
by both `max(2 source periods, applied buffer + 1 source period)` of frame age
(excluding its explicit decode wait) and `min(4 ms, half a source period)`
beyond the otherwise safe target. This is temporary recovery time, separate
from the half-frame Low Latency adaptive-buffer cap and the independently
selected Reduce judder retiming budget. The original source targets remain
anchored so an isolated late frame does not permanently move the timeline.

New sessions no longer discard a frame merely because the presentation floor
pushes it more than half a source period. Queue capacity, early queue expiry,
and pre-render age rejection still shed sustained overload when a successor
exists; a lone late frame and completed offscreen work remain protected.
The worker and queue simulator select this rule through the recorded recovery
parameter. The simulator still cannot model early queue pruning or changed
GPU/decode service, so it is not live-throughput proof.

Historical snapshots default `playout_late_recovery` to zero, retaining
queue-age-only arming, the old 1 ms catch-up hold, and floor-debt rejection.
The new parameter and preset buffer ratio are included in calibration identity.
No application repeats, VRR enable/disable transitions, swapchain mode changes,
or physical OLED flicker correction are introduced. This can reduce avoidable
submission-interval compression; physical refresh behavior and brightness
stability require a fresh live display test.

Validation on the Linux native build: ten deterministic suites and replay help
pass; single-frame, warm-history, decode-contention, and early-expiry worker
fixtures pass exact replay. The 15 scenarios in
`tests/vrr/configs/late-frame-recovery-stress.json` cover all presets with
nominal, decision, preparation, submission, and scheduler faults on the 60 FPS
warm fixture; all pass interval safety and 30 ms p99 latency bounds without
saturation. Application build and offscreen help pass. The latest completed
`20261001-222806-2037694` capture has valid sequence accounting but fails the
strict exact gate, so its policy comparison is exploratory only: half-frame
recovery improves interval jerk versus half-frame buffering alone, while the
previous one-frame buffer remains more even. No native Windows deployment,
live gameplay smoothness, or optical flicker result is established. Detailed
checks and capture identity are in `build/late-frame-recovery/validation.json`.

Client warnings sample fresh pacing drops/late-preparation counters once per
reporting interval, independently of the performance overlay. A buffer at its
limit without fresh late/drop evidence does not warn. Sustained qualified
one-second service overload has a distinct warning and never suggests more
buffering. Both client warnings require the buffer to be at its maximum and
the displayed measured smoothness score to be at or below 99%, regardless of
preset. Leaving the cap or recovering above 99% hides them immediately.
Missing qualification also suppresses them. Warnings require three seconds of startup and two seconds of
persistent evidence, clear after five seconds without that evidence, and have
a thirty-second repeat cooldown. Reporting gaps over 2.5 seconds restart
qualification. They follow the existing connection-quality-warning preference.
HEVC is suggested only for active AV1 with an initialization-time hardware
HEVC probe matching the stream's HDR/chroma/resolution; Smooth is suggested
only for a capped buffer when its configured allowance is below four frames. No setting changes
automatically. Diagnostic `serviceOverloaded` does not change buffer control.

Network and client messages retain independent status sources. Mouse-mode text
has display priority while retaining both warnings; clearing any source cannot
clear the others. Client pacing counters do not feed the network frame-gap
counter or the transport connection callback. Those existing delivery-loss
signals do not diagnose a specific network component or internal GPU cause.

PyroWave packet-loss warning (2026-10-05, `3cb67d3e` plus this worktree):
the decoder observes `BUFFER_TYPE_LOST` payload holes before decoding or
stale-frame shedding, independently of the stats overlay and VRR selection.
PyroWave depacketization preserves one entry per video data packet, including
lost placeholders. The observer sums missing and total packet counts across all
delivered frames, rather than counting frames with any missing detail or averaging
per-frame percentages. Following the transport warning's numerical thresholds,
a three-second window with at least 30% missing packets, or two consecutive
windows with at least 15%, shows red text:
"Severe packet loss detected / Reduce bitrate to prevent shimmering".
A window at or below 5% clears this source; a reporting gap over 2.5 seconds,
decoder reset, or disabled connection warnings restarts qualification. This is
the percentage of video data packets missing from delivered frames after FEC,
not a raw network packet-loss percentage or a diagnosis of where packets
disappeared. Fully recovered FEC packets do not qualify, and parity packets and
wholly dropped frames are outside this observer's denominator. The transport's
whole-frame-loss callback retains the ordinary slow/poor connection text;
it cannot trigger the shimmering message. Both sources retain their own state,
alongside client pacing warnings;
mouse-mode priority is unchanged. No bitrate changes automatically.

### Cross-platform ownership and buffer-attribution correction (2026-09-19)

Production source-clock mapping now uses immutable `decoderOutputUs`. A worker or
backend completion wait is local service after decoder output; changing when that
wait is polled must not move the RTP-to-client clock offset, source slot, stale-age
origin, or cadence state. `decodeCompleteUs` remains a conservative readiness
observation for historical policies, and `decodeSyncWaitUs` records explicit CPU
waiting separately. The old `decoderOutputUs + residualWaitUs` construction was
self-dependent: queue residence could shorten a residual wait without changing
the real completion time. A material wait now records the post-wait clock as an
upper completion bound instead. The three new controls default to zero when
absent so old captures retain their recorded mapping, service and release rules:
`playout_source_mapping_decoder_output`, `playout_serial_service_gate`, and
`playout_recent_pressure_release`.

The live interval buffer now separates delivery variation from serial local
service. Serial service includes the explicit decoder wait, preparation work
excluding swapchain acquisition, and render-scheduler delay; a deferred backend
completion contributes its actual residual CPU wait in revision 2. Revision 1
used the conservative full preparation-to-observation upper bound. Growth is eligible only when the attributed frame is late and the qualified
window has capacity for measured serial service and decoder-queue pressure.
Revision 1 tested only one pair; revision 2 uses the total smoothed intended
interval time over the same one-second window as interval pressure.
Extra standing delay cannot make a pipeline whose serial work exceeds its slot
process frames faster. The preset's long severity-weighted history remains part
of quality reporting and attack qualification, while only recent current pressure
with a below-target long score renews the clean-time release hold. Old below-target score debt therefore no
longer pins live delay after the recent disturbance ends; historical policies
retain the former hold behavior.

Windows queues the frame's decode dependency on the GPU, records the backbuffer,
signals and flushes a present-ready fence during preparation, then lets the GPU
run during the worker's cadence hold. At the target boundary it verifies the exact
fence value and waits only for any residual work before `Present`. The
presentation lock is released during both the cadence hold and that residual
wait, and the source `AVFrame` stays owned through presentation. Since
2026-09-22 that lock is not FFmpeg's decode lock on separate devices, and a
monitored decode fence is waited on before scheduling (see the 2026-09-22
section above). Linux VAAPI keeps
one explicit worker readiness synchronization but removes the duplicate explicit
prepare-time synchronization. Hardware Vulkan preparation retains the imported
source mapping until GPU completion. The VAAPI Mailbox path now uses libplacebo's
render-complete presentation semaphore without an additional CPU output wait.
Other presentation modes, imports and software frames retain the bounded output-completion poll.
Both platforms spend the existing playout interval on preparation, with
`playout_prepare_on_arrival=1` and `render_start_after_submission_us=0`.
This is necessary for the asynchronous path: without a CPU completion sample,
it cannot rely on that sample to learn an adequate GPU render-ahead allowance.
Presentation targets, buffer limits and native interval protection are unchanged.

These changes improve overlap and prevent unabsorbable local work from buying
more buffer; they do not prove lower visible latency or smoother scanout. Windows
fence poll/event timestamps are conservative CPU observation bounds, and a fence
first checked at the target can have completed earlier during the cadence hold.
Linux source retirement proves that imported reads can be released. The fallback
output poll supplies a separate CPU completion upper bound. Neither observation
is a hardware timestamp, and a Windows completion first observed at the target
can conservatively overstate service. Passing the service gate is not proof of
complete GPU throughput headroom. Native display feedback and physical scanout
remain separate evidence.

### Cadence qualification correction (2026-09-19)

Production records `playout_smoothing_windowed_cadence=2`. Reduce judder
qualifies a rolling four-interval mean within 25% of the fitted source period.
There is no additional recovery timeout (`playout_smoothing_recovery_us=0`);
the source-rate detector retains its independent transition confirmation.
This replaces the single-interval/compensating-pair stability gate, whose
threshold was repeatedly crossed by 90 kHz RTP rounding near 6.25/10.42 ms
at 120 FPS, especially when normal intervals interrupted alternating pairs.
Revision 2 also tolerates one RTP tick of rounding at the half-period bound
and a short interval down to one quarter-period when the preceding long
interval compensates it and their mean is within 25% of the fitted period.

Missing frames, invalid source timing, phase/epoch discontinuities, detected
rate changes, uncompensated bursts and intervals above 2.5 periods discard
qualification evidence. Four new consecutive intervals are required. Storage
is fixed; this does not wait for future frames or add a frame queue.
Historical captures default to revision 0 and a 200 ms recovery timeout;
revision 1 retains the four-interval gate with the original burst bound.
Both retain their original integer period updates for exact replay.

The selected blend keeps 85% of the predicted slot and 15% of the raw slot,
with a 2.5% period EMA. Revision 2 retains fractional EMA updates so a slow
filter cannot leave permanent period error after a source-rate change.
Positive retiming remains capped at 2 ms. The cadence correction itself leaves
preset buffer caps, quality targets, holds and release rates unchanged; the
current policy's later Balanced release adjustment is described with the live
interval queue below. Calibration identity includes all five
smoothing parameters so old readiness profiles cannot cross-seed this policy.

Replay now audits first-frame GPU readiness using the row's captured responsive
revision before constructing the controller. Previously that one row could be
checked with the older queue-inclusive rule, falsely rejecting valid output-
plus-blocking-wait timestamps. The timestamp constraints are unchanged.

Validation: all six required deterministic VRR suites, diagnostics and overlay
checks pass; the native application builds and passes offscreen help. The latest
Balanced, preceding Smooth and earlier Balanced captures from the September 19
20:53 run pass exact historical replay, as do new and revision-1 cold/warm and
first-frame GPU-wait fixtures. Eleven diagnostic-tampering checks remain
rejected. Final rebuilt replay matches the selected override to the production
resolver on every capture.

On the latest Balanced capture, presented jerk above 2 ms falls from 66.6% in
the original recording to 25.0% with revision 1, then 10.2% with this policy.
Against revision 1, median jerk falls from 1.540 to 0.393 ms and mean decoder-
output-to-submission latency from 10.189 to 9.806 ms. p99 latency is essentially
unchanged (13.173 to 13.182 ms); p99.95 remains 24.684 ms. Rare jerk tails do
not uniformly improve: p99.95 increases from 10.300 to 12.148 ms. Preceding
Smooth improves from 26.9% to 5.8% over 2 ms; earlier Balanced from 34.9% to
17.4%, with a 0.236 ms mean latency increase in that backlogged capture. These
are within-capture policy comparisons, not matched gameplay comparisons
between presets.

Four nominal/fault scenarios pass zero modeled interval violations, 16 ms
reserve and 20 ms p99 latency bounds without worker saturation. These are
fixed-admission controller results, not live GPU or physical scanout validation;
the captures have no usable raster phase coverage. Input identities, full
metrics, tradeoffs and final build hashes are in
`build/judder-optimization-20260919/validation.md`; reusable variants are in
`tests/vrr/configs/windowed-cadence-variants.json`.

### Source-offset transition recovery (2026-09-15)

Live Windows and Linux sessions now reject cadence-ineligible clock-offset
observations. A source-phase discontinuity retires the old minimum-observation
window while retaining the applied offset and the interval buffer. Subsequent
eligible observations recover at 2400 us per second of monotonic sender time,
with a 100 us per-observation cap. This retains the former 20 us/frame correction
rate at 120 FPS without making 60 FPS converge twice as slowly. Fractional credit
is retained, but rejected observations and capped stalls cannot bank future
catch-up steps. A genuine epoch reset clears the clock and fractional state.

Production now observes immutable decoder-output-minus-RTP. Unwrapped RTP time
ages the window and sets the correction budget, so worker queue residence,
decoder-fence poll timing, renderer work, and GPU completion waits cannot feed
back into the source-clock mapping. FFmpeg decoder service can still affect when
`decoderOutputUs` is sampled; the windowed minimum is an empirical mapping rather
than a host/client clock synchronization. Captures from the prior decode-complete
and worker-clock implementations retain those behaviors explicitly for exact replay. A cadence break ends
startup's unrestricted downward warmup instead of restarting it. No presentation
target is changed after preparation starts. The display-period startup clamp,
queue capacity, preset delay caps and release rates are unchanged. This can move
a genuinely late source phase later; it is not a zero-latency cure for unfinished
work and does not establish optical tear freedom. Vulkan's persistent native
modes and software safety floor remain unchanged.

`playout_offset_cadence_gate=0`, `playout_offset_slew_us_per_second=0`, and
`playout_offset_source_clock=0`
retain the historical observation and per-frame-slew path when absent from old
captures. New sessions capture all three switches and `playout_offset_maximum_step_us`.
See [offset-recovery investigation](docs/vrr-offset-recovery.md) for the supplied
trace evidence, implementation tradeoffs and pending validation. No build,
regression suite, exact replay or live A/B was run for this follow-up.

### Adaptive presentation permission (2026-09-15)

Active VRR now owns its native presentation policy instead of exposing a
separate `Allow tearing` preference. Loading preferences removes the retired
`allowvrrtearing` key so an old profile cannot silently disable adaptive
presentation. V-Sync remains the user-facing prerequisite for VRR; it does not
remove the allow-tearing capability that DXGI VRR requires.

On Windows DXGI, adaptive frames always use
`Present(0, DXGI_PRESENT_ALLOW_TEARING)`. Tight or unsafe slots retain the
controller's per-frame `Present(1, 0)` protection, so removing the preference
does not remove synchronized late-frame handling. The swapchain still requires
the allow-tearing capability. Existing enabled-policy calibration identities
remain stable.

On Linux Vulkan, the renderer always selects the qualified adaptive mode for
the surface: Mailbox on ordinary Wayland, Immediate on supported X11/KMSDRM or
Gamescope, and the existing Gamescope Mailbox/FIFO compatibility choices.
There is no longer a user-selected tear-free Mailbox/FIFO branch.

Schema-5 retains `session_allow_tearing` for capture compatibility. New live
sessions always record it as enabled. Replay still honors an explicit false
value from an older capture, treats an absent historical field as enabled, and
audits the native arguments of that recorded policy exactly.

### Retired oscillating latency test (removed 2026-09-11)

The temporary oscillation checkbox, saved preference, session plumbing and
worker timer have been removed. The selected latency preset stays fixed until
reconnect. Saving preferences removes `vrrlatencyoscillation` from older settings.
Trace columns `session_latency_oscillation` and `latency_test_phase` remain zero
for schema compatibility. The report tool retains historical phase parsing so
existing captures remain readable; historical oscillation reports are not exact
fixed-policy A/B evidence. Calibration follows the selected preset normally.

### Windows buffer regression investigation (2026-09-11)

The user-confirmed 4K/116 FPS Balanced session at 18:52:58 recorded 39 GPU-ready
event timeouts, followed by decoder recreation, approximately every ten seconds.
The newest completed connection is `Moonlight-vrr-20260911-185258-376.vrrtrace`
(259,093 bytes, SHA-256
`CA2D3824F96865F8F30BC7EACCE70BE70DA7ABBF636333875EEB28D4EF2C51B5`).
Its 858 arrivals pass exact replay before and after this correction. Its final
8.24 seconds contain no source intervals above 25 ms, but 11% of presented
interval pairs have jerk above 2 ms. Earlier connection fragments from this
same application run supply the timeout evidence; they are not mixed into the
newest connection's baseline metrics.

Two reproducible controller defects are corrected in responsive revision 2:
compensating source jitter no longer permanently disables smoothing, and early
readiness slack pays for an advanced deadline before acquiring extra reserve.
All three preset fixtures reduce average jerk from 8 ms to about 2.95 ms for
9/17 ms source pairs. Clean desktop transition bounds and recovery still pass.
The original final 8.24-second segment's candidate replay is unchanged because these defects
are not triggered there; it must not be cited as a measured gameplay improvement.

Windows now validates fence values between short event waits rather than
depending on one event notification for readiness. Deterministic missing-event,
stale-event, timeout and device-removal tests pass. A local GPU-copy probe passed
1,000 asynchronous fence completions, including 500 suppressed event waits.
The original single-event wait did not reproduce the periodic failure in that
standalone probe. A subsequent 64.94-second gameplay check
(`Moonlight-vrr-20260911-192230-869.vrrtrace`, SHA-256
`47D95A875F91A7023C0EAC938B3483E74D6DC724B3CBE049CA9CEB24A3C5325F`)
recorded no GPU-fence timeouts or decoder recreation; the user reported noticeably
better motion. Readiness was still 83.9%, with mean preparation 5.96 ms, including
5.48 ms in the render-ready wait. It lost arrival row 2 at startup (footer:
6,975 allocated, 6,974 recorded, one dropped); exact replay correctly fails on
frame 35. Do not treat this incomplete capture as strict A/B proof.

The follow-up Windows change implements the existing pre-schedule decode wait
using the frame's captured D2R fence. Previously Windows inherited the no-op
implementation, so asynchronous decode waiting was mixed into rendering service
and its predictor. The same bounded, fence-verified wait is used, without holding
the decode context lock. Monitored fences use the shared worker event as a wake
hint; non-monitored fences use short sleeps and completion polls. Failed waits
invalidate adaptive preparation and request recovery without advertising readiness.
The interface overload retains existing frame-based Linux readiness behavior.
New per-frame capture data must establish how much of the former render wait was
decode work; moving the boundary alone is not proof of lower total latency.
The follow-up app and diagnostics were rebuilt, all six deterministic suites
passed, fresh single-frame and warm-history fixtures passed exact replay, and
five fault scenarios passed their interval and latency assertions. The original
complete capture also retained exact historical replay. ChaseShare and its ZIP
were updated and hash-verified; the installed follow-up executable SHA-256 is
`0A52F9DAA1A2D4E77F125EABB86FFDAB4BF93D8F9AB816F38E5D180A36BF83DA`.
The deployed UNC replay executable passed its runtime help smoke test.

The subsequent 117.71-second capture,
`Moonlight-vrr-20260911-193700-084.vrrtrace` (3,740,216 bytes, last write
2026-09-12 00:39:09 UTC, SHA-256
`2AAF8DC583805162CC7A4D17E0958D40151CDE39BF80A4C78E66F5256919DB24`),
contains all 12,824 arrivals and 12,816 presentations. It records no fence
timeouts; the user confirmed the spikes were gone and motion was much smoother.
Mean GPU decode wait is 5.47 ms; preparation is 1.34 ms (0.91 ms render-fence
wait). Full decoder-output-to-submission latency is 12.67 ms mean / 16.91 ms p99.
Presented jerk is 6.045 ms p99, with 11.9% of pairs over 2 ms; sender-spacing
error is 4.402 ms p99. Three raw sender intervals exceed 25 ms. These remain
separate from client readiness and do not establish optical display smoothness.

The user identified the fluctuating overlay score as `Client ready on time`.
Its global value is 74.9%; the overlay combines the current and preceding
one-second windows and treats any preparation completion after the target as
late. Among 3,215 late presentations the median miss is 64 us, p95 663 us,
and p99 1,226 us. The buffer is held at its Balanced 8,621 us cap; this score's
variation is not evidence of oscillating buffer depth. Its strict deadline
definition is retained, and it must not be called a visual smoothness score.

This capture exposed two replay bugs: a startup idle estimate shifted one busy
frame despite unchanged submissions, and the timestamp audit confused GPU
readiness with immutable CPU output. The corrected replay caps the idle floor
by the row's own evidence, validates both timing boundaries and the explicit
decode wait, and measures latency from immutable output. All targets,
submissions, tear/raster classes, refresh phases and required controller fields
now reproduce exactly, with zero missing arrivals and exit code zero. The
original launcher sidecar retains the pre-correction failure; the fresh exact
result is `build/buffer-readiness-complete-baseline.json`.
Its verified share copy is `Moonlight-vrr-20260911-193700-084-replay-verified.json`.
Final replay SHA-256 is
`DCA1A60C6E88D31EE53AC8A3DAC79A483F437DDE6C2228267C98F214A184058F`;
the diagnostic executable and refreshed ZIP were republished and hash-verified.
Final session-policy comparison is identical to the capture; the five stress
scenarios pass with zero modeled interval violations and 16.91--18.41 ms p99
decoder-output-to-submission latency. No scenario is worker-saturated.

### Radeon 890M timeout diagnostics (2026-09-14)

The supplied `Downloads/Moonlight-1789408757.log` (65,618 bytes, SHA-256
`677B3D4A0DCF8B75649E52C360BB0B1E40E9501ED0658A341515F96E89669E72`)
records a present-ready timeout at target 16504, completed 16503, followed by
decoder recreation. Three decode queue overflows precede that timeout and ten
follow it during 00:10:30--00:10:40. No accompanying VRR capture was found in
Downloads. This report does not establish which GPU dependency stalled.

Failure logs now distinguish the elapsed deadline, iteration guard, reversed
clock, native wait failure and device removal, and include elapsed time and
wait-call count. Cross-device Signal/Wait failures include HRESULT and target.
Present-ready failures additionally report signal/flush/event-setup duration,
context-lock reacquisition duration, both device removal reasons, device/texture
mode, the frame's captured decode target, and both views of the shared fences.
Fence snapshots are sequential observations after reacquiring the presentation lock;
the next-signal counters may include newer decode work and are not the failing
frame's target. A zero captured target means no captured boundary is available.
The existing 50 ms budget, 100-wait guard, synchronization ordering, error
recovery and presentation policy are unchanged. These are diagnostic additions,
not a demonstrated correction for the reported GPU stall. A same-build comparison
with `D3D11VA_FORCE_SEPARATE_DEVICES=0` and deep tracing is still required to test
the shared-device/copy path against the separate-device/bind path.

### Production interval-quality queue (promoted from V2)

Every normal VRR session now selects revision 7 without an A/B setting. The queue
uses 0.5 ms tolerance for Low Latency and Balanced Target and 0.25 ms for Smooth;
explicit historical revisions retain their recorded tolerances. Preset
targets and severity weighting remain active.
Revision 7 retains revision 6's interval measurement and replaces its
binary score with severity-weighted quality tied to each latency preset.
Revision 6 superseded the revision-5 conditional-lateness measurement
described below. For consecutive submitted frames, intended spacing is the
difference in mapped source time plus deliberate Reduce judder adjustment.
The measured residual is `abs(submissionInterval - intendedInterval)`, including
zero-error intervals. Constant latency offsets cancel, and host cadence changes
are removed before scoring. Submission boundaries are a display-timing proxy,
not optical scanout confirmation. Discontinuous/missing frames break the pair;
their drops remain separately visible.

The controller and overlay share a one-second average (10 ms buckets). After
initial qualification (500 ms and 32 intervals), or one-second requalification
following a later sequence break, interval error through the selected profile tolerance
is accepted (0.5 ms for Low Latency/Balanced Target, 0.25 ms for Smooth). For each
evaluated interval, revisions 9/10 report
`loss = clamp(max(intervalErrorUs - toleranceUs, 0) / intendedIntervalUs, 0, 1)`.
Production revision 7 instead uses `meanErrorUs` for both reporting and control.
Explicit revision 8 uses the same arithmetic with its historical tolerance.
The reported score is `100 * (1 - sum(actualIntervalUs * loss) / sum(actualIntervalUs))`
over the selected preset's one/two/five-minute history, using 100 ms buckets.
Loss retains fractional microseconds rather than rounding every frame. Missing
coverage is unknown; before the selected history duration, the score uses the
available evaluated time. This is timing quality, not a percentage of perfect
frames or a perceptually calibrated score.
Historical revision 10 separately accumulates buffer-control loss using the qualified
one-second `meanErrorUs` in place of `intervalErrorUs` in the loss formula.
It weights that loss over the same preset history and evaluated intervals.
Growth qualification, current pressure, attack amount, hold renewal, clean
recovery and release all use this relaxed signal. The accurate reported score
can stay below target while the buffer releases; it is not the control score.
Revision 9 retains its coupled per-interval reporting/control for exact replay.

Low Latency / Balanced Target / Smooth seek 99% / 99.5% / 99.99%, respectively,
over one / two / five minutes.
An attack requires the preset-duration buffer-control score below its target, current mean-based loss
above the preset's allowed loss, and a fresh interval error over the selected
tolerance with
readiness-attributable lateness. It acquires only the mean-based excess above
the preset allowance (`(1 - target) * intendedIntervalUs`), bounded by fresh
error above tolerance, the affected frame's lateness, 250 us per 250 ms, and
125 us applied per frame. The attributed frame must also be absorbable: its
decoder-queue time and complete serial service must each fit the actual intended
target interval. Serial service is the explicit decoder wait plus preparation
excluding acquisition plus render-scheduler delay, conservatively enlarged by a
deferred GPU-completion upper bound when one exists. This uses the target-to-
target interval after Reduce judder adjustment, rather than assuming the fitted
source period is always the available slot. Old score debt alone cannot authorize
buffer growth, and work that cannot fit a slot cannot be repaired by adding
standing delay.

Current attributable pressure renews the protection hold and clears fractional
release credit only while the preset-duration buffer-control score is below its target.
Production records `playout_hold_renew_below_target=3`: score changes in either
direction at or above the target neither restart the hold nor pause qualified
recovery or gradual release. An above-target capacity dip pauses earning
recovery and release while service cannot fit, but preserves earned recovery
instead of restarting the timer. Revision 2 retained that capacity-reset
behavior; revision 3 removes it above target. Revision 1 avoided hold renewal above target but
still paused recovery; revision 0 retains its earlier pressure-based hold.
Captured values preserve historical behaviors for exact replay. The long control
score qualifies a future attack, but old below-target debt does not renew the
live release hold after recent pressure clears. All current presets share an
eight-second clean hold and 250 us/second release. Older mode-specific holds
and rates remain only in historical captures. These holds retain protection
between disturbances. The release rate is a controller tradeoff, not
live visual proof. This adjustment
cannot improve a session already pinned at its buffer cap. The hold and release values
are serialized independently, so revision 7 captures retain their own settings.
The one-second detection window remains unchanged; quality uses the selected
one/two/five-minute history window. The overlay shows quality versus the selected target plus the current
one-second mean and selected tolerance. Historical revision 6 retains its binary proportion of evaluated
time within 500 us and its threshold-only buffer adaptation when selected through
explicit controller parameters. Explicit revision 8 retains its 250 us severity tolerance for historical compatibility; current production captures revision 7 and displays the configured tolerance (0.50/0.50/0.25 ms preset defaults). The earlier revision-7 regression report supported restoring the last user-confirmed smooth setting; its initial capture was a 417-row connection fragment. After normal application exit, that finalized capture contained 1,981 rows over 17.91 seconds, in Lowest latency mode, with applied buffer fixed at its 4,310 us cap and 26 playback drops. It did not demonstrate buffer oscillation or isolate tolerance as the cause of the reported motion regression.

The preference, Session presentation snapshot, decoder parameters, Pacer signature,
and VrrSessionConfig no longer carry a queue-arm flag. The existing
`|mean-miss-queue-v2` calibration suffix is retained unconditionally to preserve
previous V2 users' cache identity; its spelling is historical, not a selector.
The overlay reports smoothness and the preset target without V1/V2 arm labels.

During gameplay iteration, diagnostic validation was deferred at the user's
request. VRR16 release finalization now accepts revisions 6/7/8 in replay,
round-trips their captured parameters, and updates production capture assertions
to revision 7. Release CI runs the deterministic suites plus fresh single-frame
and warm-history exact-replay fixtures before packaging Windows diagnostics.
These are synthetic validation checks, not live A/B or optical latency evidence.

Historical revision-5 implementation and publication record (superseded):

Published to ChaseShare after the successful incremental app/diagnostic builds.
Application SHA-256: `E00649E8F5CD70E2AEA1C87F96D97D0CA413EB7CE03B1FE6ADEBF4C88223D62A`.
ZIP SHA-256: `12F1752A43A0DCCE2EA5CCCC74DE3A6744D185550465C2F8B494C6E9A129A220`.
Build/deploy/live copies and portable markers were checked. The user explicitly
requested skipping further tests until they say to finalize; no additional
replay, help smoke test or simulation was run after that instruction. The four
deterministic suites had already completed successfully before the interruption.
At that stage finalization and live A/B assessment were pending. The user has
since ended A/B selection; test and replay finalization remain separately deferred.

The former `V2 Queue` checkbox was persisted as `v2queue`, defaulted off, and was captured
once per stream through preferences, Session::PresentationSettings,
DECODER_PARAMETERS, Pacer::initialize, and VrrSessionConfig. Decoder recreation
retains that snapshot. Off resolves `playout_responsive_buffer=4`, preserving the
installed baseline; on resolves revision 5. Both use the same queue capacity,
decode/queue accounting, readiness clock correction, and preset buffer caps.
V2 calibration keys have a separate suffix, although cached history is diagnostic.

V2 uses actual preparation completion against the original deadline before
recovery clamps. It averages positive lateness among **late frames only** over
one second (10 ms buckets). On-time frames establish sample coverage but do not
enter that mean. A mean through 1000 us cannot request more buffering. Above
1000 us, a fresh late frame may request only the mean excess, capped at 250 us
per 250 ms, after at least 32 observations spanning 800 ms. The applied increase
is limited to 125 us per frame. Old samples alone cannot repeatedly request more
buffer. Discontinuities, source stalls, cancellation and sustained overload do
not supply clean adaptation evidence. Native presentation timing remains diagnostic.

V2 retains protection for 2/4/6 clean seconds and releases at 250/200/100 us per
second for Lowest/Balanced/Smoothest. These values are captured in
`playout_mean_miss_hold_us` and `playout_mean_miss_release_us_per_second`, so the
current preference cannot change exact historical replay. Silence does not
count as clean time. The existing 1 ms minimum, configured-frame caps and 16 ms
absolute ceiling remain in force.

The V2 overlay shows `Average miss (30s)` and a separate diagnostic smoothness
curve: `100 / (1 + (max(averageMissUs - 1000, 0) / 3000)^2)`. It is exactly 100
through a 1 ms average and continuous above it; it is not a measured probability
of visible smoothness. The 30-second score does not drive the one-second
controller. Client drops are reported separately and have no invented lateness.
The V1 overlay retains its historical on-time percentage/target and identifies
the selected queue arm explicitly.

User requested live gameplay A/B rather than simulated tuning. No candidate
sweep is used to select this experiment. Deterministic arithmetic/lifecycle tests
and exact diagnostic fixture checks validate implementation, not visual quality.
The earlier newest completed capture was
`\\allytwo\ChaseShare\vrr-traces\Moonlight-vrr-20260911-212200-077.vrrtrace`,
4,929,919 bytes, UTC last-write 2026-09-12 02:25:07, SHA-256
`8A65280D51E1489548A8785A28457B9C562F3D8EC6FFCD6C3BD4FEA80243BFD3`.
Fresh exact replay exited 3 on frame 10,737. Its footer records 16,255 arrivals,
16,253 enqueued rows and two dropped diagnostic rows; missing frame 10,736
prevents faithful reconstruction. Its matching launcher stderr records the same
failure. This capture is exploratory only, not A/B proof.

The tracer now uses a bounded MPSC ring instead of dropping a row whenever the
writer holds the handoff mutex. Producers publish completed row copies without
waiting for the writer. Capacity exhaustion or bounded producer contention still
drops diagnostics explicitly. Three-producer tests check 60,000 rows for intact
contents and per-producer ordering; shutdown drains in-flight publication.

### Historical baseline preset on-time targets and thresholded misses (2026-09-11)

Responsive revision 4 selects 99% / 99.5% / 99.95% readiness targets and
30 / 60 / 120-second learning windows for Lowest latency / Balanced / Smoothest.
The displayed outcome window is 30 seconds for every preset. Lateness through
1 ms is on time. Lateness over 1 ms through 2 ms becomes buffer pressure and an
outcome miss only when more than half of the live window exceeds 1 ms. Any
lateness over 2 ms and every client drop is a hard miss. The overlay continues
to expose lateness over 1 ms and 2 ms and a capacity indicator. Existing preset
caps and the 16 ms absolute ceiling still apply.

Revision 4 also fixes the readiness clock boundary. The worker used to replace
`decode_complete_us` with the wall clock after its GPU fence wait. If a decoded
frame had already waited in the pacing queue, that queue residence was therefore
reported as decode work. More buffer created more queue residence, which raised
the learned requirement and produced a positive feedback loop. The worker now
adds only the measured blocking fence wait to immutable `decoder_output_us`.
Transport queue age and the displayed queue-delay metric retain their separate
origins. Revision 3 and earlier keep their recorded behavior during replay.
That revision-4 arithmetic is historical in current source: production now
maps directly from decoder output and records a material wait's post-wait clock
only as a conservative readiness observation.

The exact capture used to prove the loop was
`C:\Users\Chase\vrr-traces\Moonlight-vrr-20260911-202703-700.vrrtrace`,
8,375,449 bytes, last write 2026-09-12 01:35:46 UTC, SHA-256
`D967B40BFD6676A52FE58F5FF0963572C71F9FC6DACBFFD1E849F60A492F2700`.
It contains 27,707 delivered frames with exact replay and complete sequence
integrity. In its last 30 seconds, strict accounting reported 96.79% on time;
only 39 of 3,210 outcomes were over 1 ms and 12 were over 2 ms. The old
readiness boundary included about 5.39 ms mean pacing-queue residence and drove
the applied buffer to its 16 ms ceiling. Reconstructing the boundary from
decoder output plus the measured fence wait removes that self-induced input.

The newest completed capture at final validation was
`C:\Users\Chase\vrr-traces\Moonlight-vrr-20260911-210959-966.vrrtrace`,
7,698,997 bytes, last write 2026-09-12 02:14:45 UTC, SHA-256
`9304641A0A24251DAA71B94373E23FEEEA6173F6BAEB80A878D0371FCA8B4214`.
Its exact replay exits 3 on frame 13,134 and the launcher produced no successful
sidecar, so it remains exploratory. Historical compatibility was instead gated
against the exact 27,707-frame capture named above. The four required
deterministic suites and replay help pass with the final diagnostic build, both
fresh schema-5 revision-4 fixtures pass exact replay with complete sequences,
and all five fault scenarios pass interval and latency assertions without worker
saturation. Gameplay validation is pending.

The iterative application build, complete ChaseShare tree and ZIP are published
and hash-verified. SHA-256 values are application
`74BBA6A97424629D831793A34CDF55F9FC3FAD391A45194BA837B1404FCABD37`,
replay
`FC58B6A30F705263AF320702AB81FB22FC550FFA6064A8960704CAC09A7EA2BC`,
queue simulator
`A7E34EA1C8F3B9037B9AE5C9F7FEEBA08CC3175F952126176B7A75F73B1DDD36`,
and ZIP
`AB069FF0A8885717774F4CE59C66626269A95E77591AA5632B96A29EA21A4D52`.
The deployed UNC replay help check passes; both diagnostic launchers describe
revision 4 and retain their existing local capture and upload behavior.

### Historical per-frame Gamescope repaint experiment (retired 2026-09-10)

The retired Linux checkbox **Test Gamescope repaint after each frame**
(`gamescoperepaint`, default off) is snapshotted into decoder parameters on
connection. With `GAMESCOPE_WAYLAND_DISPLAY` present, Vulkan creates a private
Wayland connection on a helper thread. Successful adaptive and legacy video
submissions enqueue `debug_force_repaint`; cancelled/failed submissions do not.
The render thread only sets a coalesced atomic notification and writes a
nonblocking eventfd. It never runs a process or waits for a compositor reply.

The helper keeps one command outstanding and one pending notification. Slow
replies coalesce requests; discovery/acknowledgement timeout, protocol rejection,
or disconnect disables the helper for the stream with a warning. Teardown wakes
and joins the helper without a Wayland roundtrip. Logged acknowledgements prove
command execution, not scanout or motion improvement. This private version-1
protocol is experimental and may change with Gamescope.

This command sets Gamescope's base repaint flag, not its non-base-plane overlay
flag; it cannot bypass an in-flight flip or reproduce all Steam overlay policy.
The user reports forced composition did not fix the symptom. Per-frame repaint
was a separate experiment; its user-facing control is now removed and no visual remedy is established. Keep the
forced-composition checkbox off when testing this path to avoid its independent
startup validation failure. No fixed-rate timer or extra image submission is used.

Historical update on 2026-09-09, based on `b60fa11a`: Windows VRR automatically used
the composition presentation API on Windows 11 build 22000.194 or newer when
the driver supports independent flip. Present IDs and independent-flip display
events provide native feedback. Unsupported systems retain DXGI presentation.
Submission intervals supply the production client-cadence estimate regardless
of display-event availability. Native display events remain separate tracking
evidence and DXGI refresh references remain excluded from verified measurements.
No checkbox is needed.
The helper adds no refresh wait or future-frame target; actual Ally latency,
VRR behavior, HDR, and fullscreen transitions still need hardware validation.

Historical Windows presenter policy, updated on 2026-09-09 after `a5500a26`:
VRR defaults to the DXGI swapchain so the per-frame latch decision actually
selects synchronized or tearing-permitted presentation. Composition's native
ordering does not implement that switch; the 22:27 capture used composition
for all 12,799 submissions despite 2,654 logical latch transitions. This is a
renderer contract mismatch, not evidence that changing latch hysteresis will
remove the reported judder. `MOONLIGHT_VRR_COMPOSITION=1` opts into the existing
composition backend for diagnostic comparison, snapshotted at renderer setup.
DXGI retains submission-estimate cadence diagnostics; its refresh
references are not verified frame display events. Calibration identities now
include the active presenter so their readiness histories cannot cross-seed.
The capture lost one row and failed exact replay. Exploratory replay favored
retaining the current per-frame controller over rate protection or adaptive-only
spacing, but cannot model a change of native backend or prove a visual remedy.
A fresh gameplay capture is required for that comparison. The automatic policy
below supersedes this diagnostic-only selection.

Current Windows presenter policy (2026-10-04): eligible VRR sessions prefer the
composition swapchain API on supported Windows 11/WDDM devices. Its native path
synchronizes every frame; the renderer exposes that capability to the worker,
which records `native_synchronized_presentation=1` in the controller parameters.
The controller then reports latched presentation at every source rate, including
startup and recovery, and omits the extra software display-period floor. This
corrects the historical mismatch between logical latch transitions and constant
native synchronization. Unsupported systems and setup failures retain DXGI;
`MOONLIGHT_VRR_COMPOSITION=0` explicitly selects DXGI for comparison. Neither
the source buffer nor the GPU readiness dependency is removed by this change.

Current VRR timing choices (introduced after `20fa2bc4`, allowances updated
2026-10-01): the `VRR timing`
selector offers Low Latency, Balanced Target, and Smooth throughout the VRR
frame-rate range. `vrrlatencymode` persists IDs 2, 1, and 0 respectively.
Balanced Target is the new-user default. A saved mode takes precedence;
otherwise an existing `vrrlatencyfix=true` migrates to Balanced Target and
`false` to Smooth.
The selection is snapshotted through session, decoder, and pacer setup, so
reconnect after changing it. Fixed-refresh pacing is independent of this setting.

| Timing choice | Adaptive playout-buffer cap | Stale-work allowance with a successor |
| --- | --- | --- |
| Low Latency (2) | Half a fitted source period | At least two periods; protected by applied delay |
| Balanced Target (1, default) | One fitted source period | At least two periods; protected by applied delay |
| Smooth (0) | Four fitted source periods | At least two periods; protected by applied delay |

These presets fill the customizable source-frame allowance, whose defaults set
`playout_delay_maximum_period_per_mille=500/1000/4000` for
Low Latency/Balanced/Smooth. The separate four-slot queue safety bound
uses the smaller of fitted and negotiated source periods, so a slower source
can still be clipped below its nominal profile allowance. Queue-only and
pre-render stale checks use at least `appliedDelay + sourcePeriod` for the new
release revision. Post-render checks add that delay only to admission-relative
age; target-relative age already starts after the delayed slot. Older captured
revisions retain their two-period checks.
The allowance bounds extra padding, not total latency or native queue depth.
Source-clock mapping, rendering/readiness learning, per-frame latch decisions,
and applicable display-spacing safeguards remain active in every choice.
Consistent padding reserves time for uneven delivery or preparation so a late
frame can still reach its intended slot. Less padding offers faster response
but can expose more late or skipped frames; more padding can absorb more of
that variation. It does not regularize game-driven frame intervals. Stable
delivery may look the same across choices, and no universal percentage of lost
smoothness follows from the selected allowance.

With `playout_responsive_buffer` enabled, live sessions set
`playout_delay_cap_uses_observed_period=1`, so preset caps follow the fitted
source period. A desktop transition from 120 to 19 or 30 FPS can raise the
profile allowance, but the queue bound still uses the negotiated period and
may prevent an increase in effective buffer maximum. The zero default retains the configured
stream-rate cap for historical replay. `VrrSessionConfig::latencyMode` resolves
the buffer cap into the trace/replay parameter
`playout_delay_cap_source_period_per_mille`: 500 for Low Latency, 1000 for
Balanced Target, and 4000 for Smooth. Earlier captures retain their recorded
ratios (including vrr17's 500/1000/3000). A zero schema default means an older
capture has no source-relative cap and retains its recorded behavior. The
historical `latency_fix_enabled`, `latency_fix_all_rates`, and
`latency_fix_delay_period_per_mille` fields remain recorded so old captures
replay exactly and Balanced Target/Low Latency retain their admission-age policy. The
internal session mode defaults to zero for historical tests and replay,
independently of the UI's Balanced Target default. Balanced Target and Low Latency append
`|latency-mode=1` or `|latency-mode=2` before calibration-key hashing, while
Smooth retains the ordinary key.

`VrrFrameDropPolicy` supplies the worker's discard checks. The all-arrival queue
simulation shares the later checks but does not model early queue pruning or
counterfactual decode waits. Every non-metronome profile permits replacing work older than two
fitted source periods when a newer queued successor exists. One period is
ordinary occupancy for a single worker waiting on the preceding frame and is
not a stale condition. In the lower-latency profiles age starts at pacer
admission. The worker subtracts the current frame's explicit decode wait only
for discard eligibility, preserving all pre-wait queue residence and subsequent
scheduler delay. A newly ready image must not expire merely because GPU service
took longer than the age cutoff: a newer queued image need not be ready. Age is
checked again after waiting to render, before spending GPU work. Smooth retains the
target-relative second check. The sole available frame is never discarded by
this policy. Local skips preserve the source clock and last submission. These
choices do not impose an FPS cap or change the native presenter.

Historical Latency fix checkbox (after `db596431`): `vrrlatencyfix` defaulted
off and applied the half-display-period allowance only near the refresh ceiling.
`VrrSessionConfig::latencyFix` remains for old tests and captures. With
`latency_fix_all_rates=0`, the fitted cadence still enters at the shared
near-refresh cutoff (116 FPS at 120 Hz) and exits below 114 FPS. Rejected excess
demand is clipped on exit so a near-ceiling miss cannot reappear as padding
solely because the cadence leaves that band; fresh lower-rate misses can acquire
ordinary protection. Historical enabled profiles used `|latency-fix=1`.

Native display cadence remains diagnostic. Windows, Linux, and macOS use the shared
readiness-attributed submission-interval error policy for buffer growth
(section 9.2); these are submission estimates, not confirmed scanout measurements.

[AGENTS.md](AGENTS.md) owns machine-specific build, deployment, and capture
procedures. This document owns the architecture explanation. Keep both current
when changing their respective contracts.

Updated on 2026-09-09 after `5e759232`: the precise interrupt clock is resolved
through the realtime API set. On this machine kernel32 has no direct export,
which falsely rejected composition despite driver support. Hidden-window setup
now succeeds for both 8-bit and 10-bit presentation buffers. Production restores
per-frame native protection (`playout_adaptive_only=0`, `playout_per_frame_latch=1`)
to avoid accumulating a display-period-plus-guard delay at 120 FPS / 120 Hz.
Composition can honor protected slots through its native ordering, so it now
advertises that capability as DXGI does. Hardware setup is verified; gameplay
display-event coverage and physical presentation still require a fresh session.

The follow-up black-screen correction explicitly sets the presentation surface's
source rectangle to the allocated buffer dimensions in `resize()`, including
initial setup. A successful `SetBuffer`/`Present` did not establish that area:
the unconfigured surface produced no composition or independent-flip events.
The on-screen probe reproduced zero events before the correction and hundreds
of composition events afterward; a screen capture confirms its image is visible.
Independent-flip coverage remains unverified in this desktop/overlay environment.
The hidden `--check` validates initialization only and cannot gate visible output.

The subsequent timing correction replaces the interrupt-clock reference with
QPC scaled to 100 ns using `QueryPerformanceFrequency`. Raw independent-flip
events were present, but their timestamps were rejected as future because the
interrupt-clock epoch was about 20 ms behind QPC on this machine. The old
independent-frame counter counted only accepted timestamps, concealing the cause.
Both target time and feedback now use the same QPC-derived domain; every feedback
sample is freshly correlated to the worker clock. No fixed offset is applied.
Diagnostics count raw independent events separately from rejected timestamps.
A hardware probe with this correction measured 464 of 465 steady-state submissions
and 3.85 ms p99 submission-to-display latency at 116 FPS / 120 Hz. This is native
OS evidence, not optical validation or a full gameplay latency measurement.

Linux AMD decode policy, 2026-09-10: before Qt/SDL can initialize a graphics
screen, `main()` appends `lowlatencydec` to process-local `AMD_DEBUG` when VAAPI
support is built. Existing flags are preserved (including the `R600_DEBUG`
fallback when `AMD_DEBUG` is unset). `MOONLIGHT_AMD_LOW_LATENCY_DECODE=0` disables
the automatic addition for diagnosis; it does not erase flags supplied by the
user. The local moonlight-dev wrapper forwards those variables into Distrobox.
This requests Mesa's separate hardware decode policy; FFmpeg `LOW_DELAY` was
already set and is not equivalent. Mesa 26.1.7's installed RadeonSI library
contains the option, and matching source propagates it to the VCN decode
message. Other vendors do not use this AMD option. Older drivers may ignore it.
No fence wait is removed or shortened, no global power state is changed, and
acceptance/effect by the device firmware is not confirmed by a startup log.
It may use more power; live benefit remains to be measured.

## 1. Fundamental model

Moonlight is the client. The host captures and encodes video, sends compressed
frames, and receives input. The client receives and repairs packets, assembles
compressed frames, decodes them, renders the resulting image, and submits it for
display. Audio and input have their own queues and timing paths.

VRR changes the scheduling and presentation of decoded video. It cannot create
a missing host frame, reverse network loss, or remove the time already spent
capturing, encoding, transporting, and decoding. It can absorb some variability
by delaying frames to a more regular schedule. That costs latency, so the
implementation constrains both the frame queue and the learned delay.

```text
Host capture / encode                           [outside this client]
    |
    | UDP video: RTP timestamp + NV frame/packet identity + FEC
    v
VideoReceiveThreadProc -> RtpvAddPacket -> processRtpPayload
    | packet ordering, repair, access-unit assembly, recovery
    v
Bounded compressed-frame queue
    |
    v
FFDecoder thread: pull decode unit -> avcodec_send_packet
    |                            -> avcodec_receive_frame
    | AVFrame + retained identity/timing metadata + decode GPU boundary
    v
Pacer selection
    +-- legacy queues / V-sync source / renderer
    |
    +-- VrrPacingWorker: bounded decoded-frame queue
          -> establish the backend decode dependency; record any CPU wait
          -> controller computes source slot, target, render-start deadline
          -> discard stale work when a newer frame is available
          -> wait for render start
          -> prepare GPU rendering and establish completion/ownership state
          -> release only a source the presenter marks reusable
          -> wait for target and applicable submission floor
          -> recheck lifecycle -> verify deferred readiness -> presentAdaptive
          -> submission/native feedback -> future controller decisions
          -> retire deferred source ownership only at a safe backend boundary
          -> asynchronous trace writer

Audio UDP -> audio RTP queue -> Opus -> audio-device queue
SDL input events -> input queue / sender -> host
```

Keep these separate: intended source slot, scheduled CPU submission, GPU
readiness, actual native call, OS presentation feedback, and physical scanout.
They are related observations, not interchangeable timestamps.

## 2. Source map and reading order

Paths are relative to the repository. The repeated `moonlight-common-c` directory
is intentional: the outer directory contains the qmake wrapper and the inner
directory contains the common library.

| Area | Source and entry points |
| --- | --- |
| User preferences | [streamingpreferences.cpp](app/settings/streamingpreferences.cpp): `reload()`, `save()`; [SettingsView.qml](app/gui/SettingsView.qml) |
| Session orchestration | [session.cpp](app/streaming/session.cpp): `snapshotPresentationSettings()`, `initialize()`, `drSubmitDecodeUnit()`, stream event loop |
| FPS recommendations | [vrrratepolicy.cpp](app/streaming/vrrratepolicy.cpp) |
| Protocol configuration | [Limelight.h](moonlight-common-c/moonlight-common-c/src/Limelight.h), [Connection.c](moonlight-common-c/moonlight-common-c/src/Connection.c), [SdpGenerator.c](moonlight-common-c/moonlight-common-c/src/SdpGenerator.c), [RtspConnection.c](moonlight-common-c/moonlight-common-c/src/RtspConnection.c) |
| Packet ingress | [VideoStream.c](moonlight-common-c/moonlight-common-c/src/VideoStream.c): `VideoReceiveThreadProc()`; [Video.h](moonlight-common-c/moonlight-common-c/src/Video.h) |
| Packet repair and assembly | [RtpVideoQueue.c](moonlight-common-c/moonlight-common-c/src/RtpVideoQueue.c): `RtpvAddPacket()`; [VideoDepacketizer.c](moonlight-common-c/moonlight-common-c/src/VideoDepacketizer.c): `processRtpPayload()`, `requestDecoderRefresh()` |
| Decoder | [ffmpeg.cpp](app/streaming/video/ffmpeg.cpp): `ffGetFormat()`, `submitDecodeUnit()`, decoder thread; [ffmpeg.h](app/streaming/video/ffmpeg.h) |
| Renderer abstraction | [renderer.h](app/streaming/video/ffmpeg-renderers/renderer.h): `IFFmpegRenderer` |
| Pacer mode selection | [pacer.cpp](app/streaming/video/ffmpeg-renderers/pacer/pacer.cpp) |
| Frame and presenter contracts | [vrrtypes.h](app/streaming/video/ffmpeg-renderers/pacer/vrr/vrrtypes.h), [ivrrframepresenter.h](app/streaming/video/ffmpeg-renderers/ivrrframepresenter.h) |
| VRR execution and tracing | [vrrpacingworker.cpp](app/streaming/video/ffmpeg-renderers/pacer/vrrpacingworker.cpp) |
| Deadline waiting | [vrrtargetwaiter.cpp](app/streaming/video/ffmpeg-renderers/pacer/vrr/vrrtargetwaiter.cpp) |
| Timing policy | [vrrtimingcontroller.cpp](app/streaming/video/ffmpeg-renderers/pacer/vrr/vrrtimingcontroller.cpp), [vrrtimingcontroller.h](app/streaming/video/ffmpeg-renderers/pacer/vrr/vrrtimingcontroller.h) |
| Active learning models | [prediction.h](app/streaming/video/ffmpeg-renderers/pacer/vrr/prediction.h), [reserve.h](app/streaming/video/ffmpeg-renderers/pacer/vrr/reserve.h), [smoothnessfeedback.h](app/streaming/video/ffmpeg-renderers/pacer/vrr/smoothnessfeedback.h) |
| Calibration persistence | [profile.cpp](app/streaming/video/ffmpeg-renderers/pacer/vrr/profile.cpp) |
| Windows native presentation | [d3d11va.cpp](app/streaming/video/ffmpeg-renderers/d3d11va.cpp), [d3d11composition.cpp](app/streaming/video/ffmpeg-renderers/d3d11composition.cpp) |
| macOS native presentation and display range | [vt_metal.mm](app/streaming/video/ffmpeg-renderers/vt_metal.mm), [macdisplaytiming.mm](app/streaming/video/ffmpeg-renderers/macdisplaytiming.mm) |
| PyroWave decode and framing | [pyrowavedecoder.cpp](app/streaming/video/pyrowave/pyrowavedecoder.cpp), [pyrowaveframing.cpp](app/streaming/video/pyrowave/pyrowaveframing.cpp) |
| macOS PyroWave shared output | [pyrowavemetal.mm](app/streaming/video/pyrowave/pyrowavemetal.mm) |
| macOS PyroWave calibration draw | [pyrowavemetalcalibrator.mm](app/streaming/video/pyrowave/pyrowavemetalcalibrator.mm) |
| Replay and its contract | [vrrreplay.cpp](tests/vrr/vrrreplay.cpp), [VRR test README](tests/vrr/README.md) |
| Statistics | [decoder.h](app/streaming/video/decoder.h): `VIDEO_STATS`; overlay formatting in `ffmpeg.cpp` |

For a timing change, start with the resolved session parameters, follow
`VrrPacingWorker` into `schedule()`, then follow the actual presenter call and
feedback back into the controller. Read replay only after understanding what
the live path records and which parts replay holds fixed.

## 3. Settings, negotiation, and mode selection

### 3.1 Preferences are not proof of an active mode

`StreamingPreferences` persists ordinary settings through `QSettings`.
At the inspected revision, V-sync defaults on and VRR defaults off. The VRR
timing selector defaults to Balanced Target for new users, with the saved-checkbox
migration described above. Reduce judder defaults on, preserves the saved
`smoothvrrframetiming` choice, and enables the moderate cadence smoother below.
Legacy frame pacing defaults off. The default requested stream is 720p60.
These are defaults, not evidence of the user's current saved settings.

The FPS picker is advisory. Fixed 30 and 60 FPS remain available. When V-sync
and VRR are requested, usable display refresh rates contribute VRR choices at
`floor(refresh - refresh^2 / 3600)` and Low-latency VRR choices at
`floor(refresh / 6) * 5`, restoring the original dropdown calculations
(116 and 100 FPS at 120 Hz; 138 and 120 FPS at 144 Hz). Native rates remain
available as ordinary choices with VRR enabled or disabled. Native choices take
precedence when another display's calculated recommendation matches them, and
a saved maximum-refresh selection stays selected when VRR is toggled.
A saved custom FPS remains selectable. Toggling VRR does not rewrite saved FPS;
`m_StreamConfig.fps` receives the requested preference.

`snapshotPresentationSettings()` resolves that request for the session:

1. Query the actual window display refresh. An unavailable refresh may fall
   back to 60 Hz for legacy behavior, but that fallback cannot qualify VRR.
2. Resolve effective V-sync. A requested FPS over refresh plus 5 disables it.
3. Require readable refresh, effective V-sync, and stream FPS no greater than
   display refresh for VRR.
4. Force effective desktop fullscreen when VRR is accepted, keeping the saved
   window preference intact. macOS uses native Cocoa fullscreen/Spaces for it;
   Windows/Linux retain the borderless desktop path.
5. If VRR was requested but rejected and effective V-sync remains enabled,
   enable fixed pacing even if the separate legacy pacing checkbox is off.

On macOS, the strict refresh query first uses `NSScreen.maximumFramesPerSecond`.
The FPS picker uses the same native maximum; a ProMotion mode whose SDL refresh
is zero therefore contributes its actual 120 Hz choices. Session VRR additionally
requires finite, positive native intervals and
`maximumRefreshInterval - minimumRefreshInterval > 1 us`. A fixed-refresh
screen cannot qualify merely by advertising a high maximum FPS. The window's
actual screen is queried on the main thread. Display-list queries match SDL
desktop bounds to CoreGraphics display IDs instead of assuming enumeration order.
Moving an active VRR window recreates the Metal renderer; an unavailable or
fixed range, or changed maximum refresh, disables VRR for that connection and
uses fixed pacing. Reconnect to qualify a later display again.
Before initializing SDL video, a requested Mac VRR+V-sync session forces
`SDL_HINT_VIDEO_MAC_FULLSCREEN_SPACES=1`. This overrides saved borderless/notch
presentation choices for that connection because SDL caches the hint at Cocoa
driver startup. The Metal presenter additionally requires that the Cocoa window
actually entered native fullscreen; the hint alone does not qualify playback.

The renderer must subsequently support the mode. The Pacer constructs
`VrrSessionConfig`, fixes `allowAdditionalQueuedFrame=false`, passes smoothing
settings, checks presenter support, and starts the worker.
Unsupported presentation or failed worker initialization falls back to the
legacy path. A UI checkbox alone cannot establish DXGI capability, active
adaptive presentation, or that the physical panel is varying refresh.

`tracevrrframes` defaults to false. The Settings checkbox enables diagnostic
frame tracing only, with export of the latest completed recording and a button
to open `Desktop/vrr-diagnostics`. The platform's Desktop location is resolved
through Qt, rather than hardcoding an English profile path. The session owns its
environment wrapper after acquiring the active-session semaphore, restoring it
only after decoder shutdown
and logger drain. Existing external trace launchers take precedence. Recording is
fixed for the stream, including decoder recreation; full UI/file/lifecycle
contracts are in [diagnostics](docs/vrr-diagnostics.md).

### 3.2 Host frame-limiter discovery

Vibeshine advertises optional `/serverinfo` fields `FrameLimiterSupported`,
`FrameLimiterEnabled`, `VirtualDisplayFrameLimiterEnabled` (integer booleans),
and
`FrameLimiterFpsLimitMilliHz` (zero follows stream FPS). The protocol is
brand-independent and can also be implemented by Vibepollo. Missing flags
mean no advertised integration; capabilities are ephemeral and refreshed on
host polling, including clearing them after downgrading a host.

`FrameLimiterEnabled` reports limiting for the configured host display path:
either the manual limiter is enabled with an applicable provider, or virtual
display mode is selected and automatic virtual-display limiting is enabled.
Windows does not require the manual checkbox for the automatic path. Linux
also requires a selected Linux limiter provider. The separate virtual-display
flag reports automatic policy availability, not activation on physical outputs.
These are configuration reports, not per-game proof of provider availability,
successful application, or app/client display overrides.

The game list no longer displays a frame-limiter warning. Capability discovery
remains available internally. Settings describe full-refresh streaming and the
optional lower-rate VRR choices without requiring a host limiter.

### 3.3 What the host is told

Session setup fills `STREAM_CONFIGURATION` with FPS, dimensions, bitrate,
colorspace/range, encryption, codec capabilities, and other connection choices.
`clientRefreshRateX100` is a refresh hint in Hz times 100; SDP emits it as
`x-nv-video[0].clientRefreshRateX100`. RTSP negotiation determines the actual
codec/profile using host SDP and server codec flags, with AV1/HEVC/H.264 paths.
Renderer setup receives that negotiated format before video streaming starts.

Packet size is aligned down to a 16-byte multiple for FEC. Connection setup
also applies route-dependent packet-size limits. These are transport decisions,
not VRR scheduling parameters.

When VRR presentation qualifies for the session
(`m_PresentationSettings.enableVrr`, decided before launch), `NvHTTP::startApp`
adds `clientVrrRequested=1` to the launch/resume query. Hosts that do not know
it ignore it. Vibeshine uses it for a low-latency WGC buffer and, in its
Automatic virtual-display capture mode (`vibe-test` 09b09ae8), holds the
virtual display at 1000 Hz. WGC stamps frames at DWM composition, which lands
on the virtual display's refresh grid; a 4x display at 116 FPS put every RTP
timestamp on a 2.156 ms grid (capture 20260922-195738 deep trace), while 1000 Hz
bounds that to 1 ms. Vibeshine also refines stamps from the game's DXGI Present
events at send time. This is host behavior the client only requests.

The client source proves which values it sends and how it interprets the
response. It does not prove how a particular Sunshine/GFE version captures,
timestamps, paces, or encodes in response. A refresh hint is not host/client
clock synchronization.

## 4. Packet ingress, frame assembly, and recovery

The video receive thread reads UDP into staging buffers, optionally decrypts
AES-GCM payloads, converts network-order RTP fields, and transfers packets to
`RtpvAddPacket()`. Video packets contain RTP sequence/timestamp identity plus
an NV header with stream packet index, frame index, SOF/EOF/picture-data flags,
and FEC metadata. RTP timestamps are in a 90 kHz domain.

The socket receive buffer accommodates roughly 2048 packets to absorb bursts.
Connection watchdogs detect prolonged absence of traffic or successful frames.
The receive path is not a frame scheduler: it should advance valid compressed
data without intentional presentation waiting.

`RtpVideoQueue` groups by frame and FEC block, tracks ordering and missing data,
and uses Reed-Solomon parity to recover losses when possible. Completed data
packets are delivered in order to the depacketizer. Unrecoverable gaps advance
recovery and notify the host. FEC status and frame-loss control messages should
not be described as individual RTP packet retransmission.

One frame's first receive time is reused for its packets. That gives a stable
assembly-duration boundary without taking a clock sample for every packet.
`processRtpPayload()` validates stream continuity, strips headers, identifies
frame boundaries/types, and assembles codec access units. H.264/HEVC Annex-B
NAL units become a linked buffer chain; IDR setup includes codec parameter sets.
AV1 follows its appropriate picture-data assembly path.

The resulting `DECODE_UNIT` owns the compressed buffer chain and carries
frame identity and timing metadata. Non-direct operation uses a bounded queue
of 15 decode units. Queue overflow flushes queued compressed frames and requests
IDR recovery, rather than allowing latency to grow without bound. Direct-submit
renderers can instead submit on the receive/depacketizer path, but the production
FFmpeg decoder advertises pull-renderer capability and owns its decoder thread.

Loss recovery respects codec dependencies. Dropping an arbitrary compressed
reference frame is different from discarding an already-decoded presentation
frame. Invalid packet continuity can drop the current frame and trigger IDR or
reference-frame invalidation recovery depending on capabilities. Repeated drops
eventually force IDR recovery; the inspected code has a 120-consecutive-drop
threshold. `requestDecoderRefresh()` flushes pending units and defers assembly
state reset to a suitable boundary. Completion releases the compressed buffers;
successful IDR completion establishes valid reference state.

## 5. Clock domains and latency boundaries

| Value | Domain / units | Meaning and limitation |
| --- | --- | --- |
| RTP timestamp | Host-origin 90 kHz counter | Source timing identity; wraps and must be unwrapped. Not a client wall-clock timestamp. |
| `presentationTimeUs` | Relative source presentation time, microseconds | Normally derived from RTP. Missing Sunshine PTS can use elapsed local receive time as fallback. |
| `receiveTimeUs` | Client monotonic microseconds | First packet arrival for the frame. Not host capture time. |
| `enqueueTimeUs` / reassembled time | Client monotonic microseconds | Complete compressed frame assembled/queued. |
| `decodeSubmitUs` | Client monotonic microseconds | Sampled immediately before FFmpeg packet submission. |
| `decoderOutputUs` | Client monotonic microseconds | Immutable timestamp captured immediately when FFmpeg returns the decoded frame; client-processing reporting origin. |
| `decodeCompleteUs` | Client monotonic microseconds | Post-output readiness observation; anchors production RTP-to-client mapping so hardware decode duration is absorbed in the sender offset. Linux samples it after `vaSyncSurface()`, Windows after the monitored decode-to-render fence wait. Without a wait over 200 us it equals `decoderOutputUs`, as on Windows before 2026-09-22 and on shared-device or non-monitored-fence sessions. |
| `decodeSyncWaitUs` | Elapsed client microseconds | Explicit CPU time spent by the worker on a decoder/backend completion primitive. It is serial service and latency accounting, not a source-clock timestamp. A zero value does not exclude a GPU-queued dependency. |
| Worker queue, decision, preparation, wait, submission times | Client monotonic microseconds | Distinct CPU-side lifecycle boundaries. |
| Shared fence values | GPU ordering identities | Establish dependencies/completion; not elapsed time by themselves. |
| Native DXGI QPC fields | QPC ticks plus frequency/correlation | OS timing evidence requiring identity and clock mapping. |
| Metal `MTLDrawable.presentedTime` | Core Animation seconds | OS-reported drawable presentation event, freshly correlated with `CACurrentMediaTime()` inside a bracket on the client clock; not the native call/return time or physical panel measurement. |
| Host processing latency | 1/10 millisecond units | Host-reported aggregate when present; zero means unavailable/inapplicable. |
| Audio samples | Audio stream/device cadence | Independent of video target scheduling. |
| `Reserve` internal time | Nanoseconds | Convert explicitly at the controller/model boundary. |

`LiGetMicroseconds()` calls the common platform clock. On Windows that is elapsed
QPC time from an opaque local epoch. Decoder and pacer timestamps therefore share
a monotonic domain. They are not automatically synchronized to host, GPU, audio
hardware, or panel clocks.

Useful differences are:

```text
assembly       = enqueueTimeUs       - receiveTimeUs
pre-submit     = decodeSubmitUs      - enqueueTimeUs
decode         = decoderOutputUs     - decodeSubmitUs
post-decode    = submissionTimeUs    - decoderOutputUs
client ingress = submissionTimeUs    - receiveTimeUs
client processing = presentationCallEndUs - decoderOutputUs
rendering         = preparationDurationUs + presentationCallDurationUs
queue/pacing      = client processing - rendering - explicitDecodeSyncWait
```

Check validity and ordering before subtracting. Post-decode includes queueing,
GPU dependencies, rendering/preparation, scheduler delays, deliberate pacing,
and native submission behavior. It is not simply the configured playout delay.
The performance overlay retains frame queue delay and rendering
time, without a separate client-processing row. These use the current queue/pacing
and rendering quantities for successfully presented frames, with one shared frame count.
Queue/pacing excludes the explicit worker GPU decode wait. In advanced tracing,
the two components plus the separate GPU decode synchronization line partition client processing.
Queue/pacing includes queue residence, target waits and other time outside
preparation, presentation and explicit decode synchronization. “Client processing
delay” ends when the presentation call returns. It does not include unmeasured
time from that return until the image becomes visible on the display.
On Windows with monitored fences, the worker waits for the decode fence before
scheduling, and that wait is the GPU decode synchronization line, as on Linux. The
GPU-side decode-to-render wait remains queued. The residual present-ready fence
wait occurs inside the presentation call and is not a decode-wait row.
Linux VAAPI/Vulkan Mailbox output is asynchronous and
does not report a CPU output-completion sample; other Linux imports still poll
before the target hold. These accounting identities therefore partition the
observed CPU path; they do not expose every GPU stage.
None of these differences alone measures click-to-photon or glass-to-glass
latency. RTT is a round trip, not measured one-way video delay.

The controller keeps source periods in Q16 fixed point where needed. Do not
collapse that to rounded milliseconds when reasoning about long-run drift.
RTP-to-microsecond conversion and epoch/wrap handling must be followed at the
specific use site; a converted RTP number still needs a client-clock mapping.

## 6. Decoder ownership and renderer handoff

The production FFmpeg decoder starts a dedicated `FFDecoder` thread after
renderer setup. It waits through `LiWaitForNextVideoFrame()` when no packets are
outstanding and interleaves pulling compressed input with draining FFmpeg output
when work is in flight. “Pull decoder” describes ownership of the common-library
queue; FFmpeg still uses `avcodec_send_packet()` and `avcodec_receive_frame()`.
Individual FFmpeg codecs may execute more work at either call.

`submitDecodeUnit()` requires a suitable initial IDR, tracks frame-number gaps,
copies the `LENTRY` chain into the reusable packet buffer, and sends the packet.
After successful submission it retains a metadata copy and submission timestamp
in matching queues. The original compressed payload pointers become invalid
after completion and must not be retained as frame storage.

For each output `AVFrame`, the decoder associates queued metadata with that
output, stamps decode completion, and copies frame number, RTP identity,
receive/reassembly time, and decode-submit time into the VRR frame record.
Legacy rendering uses `frame->pts` for source timing and a local `pkt_dts` handoff
timestamp for queue measurements. Those fields should not be substituted for
the explicit VRR timing fields.

`ffGetFormat()` selects the pixel format expected by the chosen renderer and
refuses an incompatible ordinary FFmpeg fallback. Windows has DXVA2 and D3D11VA
paths; software output and other platforms use their corresponding renderers.
Hardware decode can keep image data on the GPU. An `AVFrame` being available
does not by itself mean every GPU read/write dependency has completed.

`Session::drSubmitDecodeUnit()` also protects decoder lifetime with a try-lock:
decoder destruction has main-thread/API constraints, and units can be ignored
while that lock is held, with refresh recovery after recreation. The FFmpeg
pull path is the important steady-state path for this fork.

Legacy Pacer queues drop old frames at their bounds and move frames according
to the V-sync/render path. They defer freeing a rendered frame to protect GPU
use. VRR replaces that pacing mechanism with its worker and explicit presenter
contract; it does not replace network assembly or codec reference handling.

## 7. VRR worker: queue, execution, and lifecycle

### 7.1 Queue ownership and backpressure

The VRR queue admits four waiting frames plus one active frame in every
profile (`playout_queue_frames`; Smooth alone until 2026-09-26, and 0 = the
historical three in older captures). Separately, profile playout-delay
allowances default to half, one, or four fitted source frames for Low Latency,
Balanced Target, or Smooth and can be customized. The queue limit in
`playoutQueueLimitUs()` remains a safety
bound on those allowances:
waiting frames x period, minus render lead and the full Reduce judder retiming
budget. With three frames at 116 FPS that budget was ~16.9 ms once the retiming
cap rose to 6 ms, so Smooth's 24 ms ceiling was unreachable and live overlays
showed "limit 16.86 ms" (capture 20260922-221707). The decoder pool reserves
the extra surface (`extra_hw_frames` = classic pacer outstanding frames +
`VrrLargestQueuedFrames` - `VrrMaximumQueuedFrames`). This is a
decoded-frame queue, separate from the 15-unit compressed queue and native
swapchain buffers. Do not add these counts and treat the result as a fixed
latency: the queues have different owners, lifetimes, and service rates.
Backend-held source references can outlive the active worker step without
increasing decoded-frame admission. Their limits protect decoder-surface lifetime,
not a target amount of playout buffering.

At `submit()`, the presenter captures the decode boundary before subsequent
decoder GPU work can be queued. Under the queue mutex, stopped/suspended workers
reject frames; a full queue evicts the oldest waiting frame, marks a discontinuity,
and admits the new frame. Trace/counter work occurs outside the queue lock.

The worker also sheds stale work when a fresher queued successor exists and
age/capacity criteria apply (historical snapshots also retain floor-debt
and missed-tick rejection). All non-metronome profiles use a
two-source-period age allowance. Balanced Target and Low Latency measure it from
pacer admission, while Smooth uses the scheduled target for its second
check. A lone late frame may still be shown.
This differs from throwing away compressed reference frames and does not require
resetting the codec merely because an image was not presented.

### 7.2 One normal frame

1. Wake and consume pending window notifications. Before dequeuing for a blocking
   decode wait, reject expired queue fronts only while a newer adjacent source
   frame exists. Use two periods of the larger of fitted cadence and successor
   RTP spacing (four for historical metronome); skip this check on pending rebase
   or discontinuous stamps. Record `queue_stale` without a controller decision.
   The sole image remains eligible. Then dequeue the retained frame.
2. Check stop/suspend state and establish this frame's decode dependency. Record
   any explicit CPU wait as serial service. Production at `18602b1c` maps from
   decode completion; immutable `decoderOutputUs` remains the full-latency
   origin. Historical captured parameters can instead select decoder output.
3. Ask `VrrTimingController::schedule()` for the target, render-start deadline,
   latch request, and diagnostics using the current monotonic time.
4. Apply stale replacement policy when newer work is available.
5. Wait until render start, then recheck window/display epoch and lifecycle.
6. Call the presenter's `prepareFrame()` with the captured decode dependency
   and the selected `VrrPresentRequest`. Mode changes, rendering, and image
   acquisition belong inside this measured preparation interval; intentional
   target waiting does not. D3D11 keeps its mode selection at Present; Linux
   Vulkan keeps the swapchain's startup-selected mode.
7. If preparation supplies a completed renderer wait, feed that measured interval
   into the controller's bounded readiness history. A presenter may instead
   report completion after the target hold, or leave it unavailable. Future
   frames may start rendering earlier by an available learned lead; the source
   target and native latch decision are unchanged. Failed waits and incomplete
   timing are not training samples. The worker removes an explicit preparation-
   time GPU wait from the generic learned preparation cost so one stall cannot
   inflate both budgets.
8. Handle preparation failure/cancellation. If the presenter reports
   `sourceFrameReusable`, release the decoder surface before the target wait.
9. Wait for the target, then enforce the controller's currently applicable
   earliest-submission floor with another clock read and wait if necessary.
10. Consume final lifecycle notifications immediately before the native operation.
11. Call `presentAdaptive()`, capture result and timing, and consume any deferred
   completion result before recording submission. A deferred completion can
   train future readiness lead and bound current serial service; it cannot alter
   the target already issued for this frame.
12. Trace the outcome and retain/defer frame ownership as required by the presenter.
   Backend source retirement may continue after this worker step.

With `MOONLIGHT_VRR_OFFSCREEN_PREPARATION=1` on Linux VAAPI or PyroWave/Mailbox,
after the first ordinary frame establishes the real
swapchain format, admitted frames also receive cancellable preparation tickets.
A separate thread performs decode synchronization, source import, rendering
to an offscreen texture, and output-completion polling. It owns its own
libplacebo renderer and mapping textures; it never acquires a swapchain image.
The pacing thread waits for the ticket before scheduling, then acquires the
swapchain, copies the completed output, verifies that short copy has completed,
and applies the ordinary target wait. Only after the copy does preparation of
the next image proceed, preventing its GPU work from delaying that copy.

The 2026-09-21 freeze correction is based on `abd6b82d`. A completed
preparation ticket bypasses the two pre-render stale-replacement checks:
rendering has already finished, and a newer queued source is not evidence of
a ready replacement. Queue capacity and expiry still shed waiting work, and
shutdown, suspension and output-epoch checks still cancel active work. This
prevents a render/drop loop under sustained GPU load without altering source
timestamps or hiding preparation latency. The completed live capture
`20260921-002333-70409` reproduced exactly and contained only two presentations
in 45.8 seconds, with 2,528 completed staged images rejected as stale. This was
presentation starvation rather than a mutex deadlock. A worker regression
exercises repeated 40 ms preparation with newer queued images in all three
latency modes. It fails before the correction and passes afterward; all eleven
VRR/backend/profile/GPU trace suites, replay help, native startup and the latest
capture's exact replay check pass. Evidence is in `build/freeze-fix-validation/`.
Actual GPU throughput and visible recovery still need a live retest.

The subsequent `20260921-004719-85692` session log reported 54.17 and 31.90
rendered FPS for its two 3840x2160 streams, despite 105.36 and 107.74 incoming
FPS. Offscreen preparation introduces an extra target copy and CPU-observed
completion waits before allowing the next preparation job. It is now opt-in;
the default once again renders directly into the swapchain and retains the
mapped source until GPU reads retire, using the presentation semaphore for
completion. This removes the new staging cost; recovered 4K throughput still
requires live verification. The completed-ticket starvation correction remains
in place for experimental use.

Tickets refer to the existing three waiting admissions plus the active image;
they do not add another playout queue. Eviction and lifecycle discard cancel
the corresponding ticket. The preparation queue is independently bounded to
three pending jobs and one running job. Completed textures are reused. A
cancelled job that already submitted GPU work retains its source mapping until
the output completes; a timeout/error drains outstanding GPU work before
unmapping. Shutdown interrupts ticket waits and joins preparation before
renderer destruction. An output-size, representation, or colorspace mismatch
rerenders the same source into the current swapchain instead of displaying an
image encoded for the old output. Unsupported formats retain the existing path.

The main trace appends `prepared_ahead` and `stage_*` fields to schema 5. Stage
decode, render-command, and output-ready boundaries precede the scheduling
decision and may precede pacing dequeue. The recorded pacing-thread decode wait
remains zero for these frames. Aggregate statistics include actual stage decode
wait and rendering service rather than counting them as queue residence.
The GPU sidecar adds `stage_render` and `stage_output_ready` spans. The latter
is a CPU completion observation, not a GPU execution timestamp or scanout proof.
Replay validates stage ordering independently of direct-worker readiness.
For direct-worker captures, serial-service revision 2 identifies post-wait
clock semantics independently of the source-mapping selection, fixing the
false exact-baseline rejection after Gemini restored decode-anchored mapping.
Counterfactual replay retains recorded stage readiness; it cannot predict
how changing GPU scheduling changes stage throughput or compositor service.

Preparation-stage validation (2026-09-21): the native incremental release and
offscreen startup help pass, as do eleven deterministic VRR/backend/profile/GPU
trace suites. Nine exact replay checks pass, including prepared-frame shutdown
while awaiting completion and the latest pre-change live capture
`20260920-231030-28049`. The signed playout-offset parser and direct readiness
clock audit were corrected without bypassing exactness checks. Evidence is in
`build/prepared-stage-validation/`. These tests do not exercise the complete
new GPU path in gameplay or establish lower physical judder; a matched live
capture is still required. No Windows build or ChaseShare deployment was made.

For performance reporting, the worker keeps the decoder-output timestamp
unchanged through this sequence. On a successful presentation it records the
full interval through presentation return and the sum of measured preparation
and presentation-call durations. Telemetry subtracts the explicit decode wait
before classifying the remaining time as queue/pacing. Failed and cancelled presentations retain outcome diagnostics but
do not enter any of these paired duration totals or their frame denominator.

The spacing floor is policy-dependent. In production, a frame classified as
latched can have the software floor disabled. Therefore “every submission is
at least one display period plus guard apart” is not a universal invariant.
The worker enforces the floor the controller returns. See the native latch
contract and historical-capture caveat in section 10 before inferring hardware
protection from this choice.

### 7.3 Waiting and scheduler accounting

`VrrTargetWaiter` uses the same monotonic clock as the controller. It sleeps
coarsely until a bounded active region, then polls using
`SDL_CPUPauseInstruction()` near the deadline. Unlike the previous
`std::this_thread::yield()`, this CPU hint does not voluntarily surrender the
OS timeslice just before submission. The base active region is 500 microseconds;
learned target wake lead adds at most another 500 microseconds, for a maximum
1 ms region per wait. Clock-stall escape and absolute-deadline checks remain.
Historical trace `active_yield_count` fields now count active polling steps;
the hook name remains `yield` for deterministic tests. Windows prefers a high-resolution waitable timer
with a sleep fallback. Render and target wake-delay observations feed later
decisions, with separate limits.

`VideoThreadPriority` runs once at entry on the dedicated decoder, renderer,
V-sync, VRR pacer and optional Vulkan preparation threads. Windows first
registers each with MMCSS `Playback`, at relative HIGH for deadline threads
and NORMAL for decode/render/preparation. Registration is released on the same
thread at exit; the system AVRT DLL is loaded dynamically. If unavailable,
SDL provides the platform fallback. On other platforms SDL is the primary
API: deadline threads retain the TIME_CRITICAL request but now retry HIGH on
failure, while other dedicated video threads request HIGH. Each accepted or
rejected request is logged once. These requests need no GPU-specific CPU API;
OS permissions and scheduler policy still control effectiveness. Main-thread
renderers, codec-internal workers, network, audio and the compositor retain
their existing scheduling. No process priority, affinity or system settings
are changed. Priority and polling improvements require a fresh live comparison
of submit jerk and matched display feedback; replay cannot predict scheduling
or compositor changes. The bounded active wait consumes more CPU than yielding
when another runnable thread could otherwise use that time.

A timer returning is not permission to submit early. The worker rereads time
and loops until the applicable floor has actually been reached. Conversely,
an OS deschedule can make it late despite a correct target. Trace planned
deadlines, actual wakeups, and native call boundaries separately.

### 7.4 Suspend, restore, cancellation, and shutdown

Minimize/suspend immediately clears queued work and wakes the worker. An
in-flight prepared image can be cancelled; the worker then blocks until restore
or stop. Display/window epoch changes invalidate current assumptions, reconcile
presenter state, and cause controller rebase. Session-level display changes can
recreate the renderer or disable VRR when refresh becomes different/unreadable.

Cancellation is backend-specific: some presenters may need a native submission
to release an acquired image. The worker accounts for that feedback and any
required spacing instead of assuming cancellation has no timing effect.
D3D11 cancellation unbinds its render target and does not use Present to cancel.

Shutdown sets stop state, wakes and joins the worker, discards remaining queued
frames, closes the trace, and conditionally saves calibration. The final
presenter cancellation releases retained native state. Avoid destroying a
decoder surface or native image while a GPU operation can still reference it.

## 8. Controller: source timeline and target construction

### 8.1 Resolve the live policy before reading parameters

`VRR_TIMING_PARAMETER_FIELDS` defines the shared parameter/serialization schema.
Its initializer values preserve older behaviors for replay and tests.
`vrrTimingParametersForSession()` overrides them for production. A comment or
schema default is insufficient evidence of the current session policy.
The diagnostics checkbox does not alter this session config or the resolved
timing parameters. Exact replay uses captured parameters, independent of whether
the recording was enabled through Settings or an external launcher.

The resolver enables timestamp playout, shared readiness history and adaptive
delay. Windows, Linux, and macOS use the revision-7 interval policy: averaged client-added
submission-interval error triggers growth only with attributable late work that
can fit its intended interval. The older thresholded-event policy is disabled
with `playout_readiness_hitch_threshold_us=0`. Native-hitch adaptation is disabled.
Production restores VRR14 timeline mapping anchored to decode completion (`playout_source_mapping_decoder_output=0`),
absorbing hardware decode duration into the sender offset instead of inflating client buffer delay.
It pairs this with early preparation on arrival (`playout_prepare_on_arrival=1`, `render_start_after_submission_us=0`),
spending the existing playout cushion on overlapping GPU preparation so libplacebo rendering finishes well before
the target presentation boundary.
A gate checks complete serial service for absorbability, and release is governed by recent pressure. Their
zero initializer values preserve historical replay when captures omit them.
The latency presets set independent caps; per-frame native slot protection
remains enabled.
Display smoothness feedback remains diagnostic. Historical Linux thresholded
submission-error attribution is retained for replay; live revision 7 uses the
shared interval policy described above.
Historical feedback policies remain selectable for exact replay.
It disables the retired metronome and enables preparation on arrival.
It also sets `latchedFloorDisabled=1` and disables the extra queue-mode budget.

| Production input | Value / meaning |
| --- | --- |
| Delay start seed | 6,000 us, then source/display/work/capacity scaling below |
| Delay minimum input | 1,000 us, capped by available capacity and the selected timing allowance |
| Delay maximum input | Larger of 1,000 us and the configured source-frame allowance; default 0.5/1/4 for Low Latency/Balanced/Smooth, also capped by queue capacity and the source-relative allowance |
| Start-period ratio | 950 per mille of fitted source period |
| Maximum-period ratio | Configured source-frame allowance; default 500/1000/4000 per mille for Low Latency/Balanced/Smooth |
| Initial interval calibration | At least 500 ms and 32 consecutive valid intervals; once per controller reset, not once per FPS change |
| Interval requalification after a break | One second and at least two valid intervals, after initial calibration has completed |
| Production source mapping | Decode completion (`playout_source_mapping_decoder_output=0`); absorbs hardware decode duration into the timeline offset |
| Live interval-buffer attack | Request at most 250 us per 250 ms; apply at most 125 us per frame, with current quality pressure, fresh readiness-attributed error, and serial service plus decoder queue each no longer than the actual intended interval |
| Live interval-buffer release | Shared 250 us per second after eight seconds of qualified clean observations; short sequence breaks preserve earned recovery, while long score debt remains reporting/attack evidence |
| Historical readiness attack/release inputs | 500 us attack and 10 us release; not the live revision-7 growth/release rule |
| Live preset-cap basis | Fitted source period (`playout_delay_cap_uses_observed_period=1`); captured policies retain their recorded basis |
| GPU readiness lead | Recent completed backend wait p99 plus 500 us, attacked by at most 1,000 us per sample and released at 250 us/s; unavailable asynchronous VAAPI completion does not train this term |
| GPU readiness ceiling | `min(12,000 us, fitted source period)`; target/deadline unchanged |
| Capacity telemetry | `playout_capacity_telemetry=1` exposes unclamped demand and cap pressure in live decisions |
| Smoothing gain | 150 when Reduce judder is checked; 0 when unchecked |
| Smoothing period EMA | 25 per mille with fractional carry, plus 20,000 per million phase-error feedback; active only with smoothing enabled |
| Positive smoothing lag cap | 6,000 us, shared with the readiness reserve; active only with smoothing enabled |
| Smoothing readiness reserve | p98 of the last 128 smoother-caused shortfalls minus 500 us, at most 3,000 us, +250 us per frame, released at 500 us/s; zero when unchecked |
| Smoothing readiness bound | Enabled with Reduce judder; early retiming preserves known decode readiness and the raw target's typical render allowance; smoothing-only misses cannot grow or renew the interval buffer |
| Render lead floor | 3,000 us |
| Preparation start | Use the existing playout interval (`playout_prepare_on_arrival=1`), with no additional post-submission delay |
| Minimum preparation lead input | 2,500 us |
| Future-offset reseed requirement | 3 consecutive qualifying projections |

The old `kPlayoutMaximumUs=8000` constant and nearby historical comments do not
define the live maximum. Likewise the retained `playoutDelayPercentilePerMille`
input of 1000 is not the active production history estimator.

### 8.2 Source clock mapping and cadence

The controller validates frame identity and RTP progression, handles wrap, and
maintains an unwrapped source timeline. Backward/invalid movement, discontinuity,
or an excessive forward interval can rebase it. Source period is fitted from
sender span divided by frame span, retaining Q16 precision and using frame-number
deltas so locally skipped frames do not become an artificial slower source.
Negotiated FPS supplies fallback timing and bounds the fitted source rate.

Cadence history is bounded (6 minimum and 512 maximum samples by schema). Loose
and tight windows are 350 ms and 1 s. A major departure uses a provisional rate
candidate; production requires at least three candidate samples spanning 200 ms
before accepting a sustained change. A subsequent interval must return within
5:4 of the old fitted period to cancel the candidate; captured policies retain
their recorded ratio. Isolated gaps should not temporarily turn a high-rate
stream into a low-rate stream and resize every dependent budget.

With usable RTP, the source slot is:

```text
sourceTime = unwrappedRtpInMicroseconds + appliedClockOffset
production offset observation = decodeCompletionUs - unwrappedRtpInMicroseconds
```

`observePlayoutOffset()` tracks a windowed minimum of these observations, with
warmup and bounded slewing. The inherited offset window is 3 seconds and epoch
warmup is 64 samples. Historical captures can select `decodeCompleteUs`, use all
observations, and slew by 20 us per frame. Live sessions retire the observation window on phase discontinuities,
exclude ineligible samples, and preserve the applied offset rather than
re-anchoring it. They use 2400 us per second of worker decision time, capped at
100 us per observation; the raw mapping observation itself is unchanged. This prevents
an old phase minimum from steering the new phase and avoids FPS-dependent
steady-state convergence. The minimum remains an empirical client mapping,
not measured host capture latency or absolute host/client synchronization.

Timestamp mode zeroes the separate legacy readiness reserve/phase demand:
the timestamp playout delay is the jitter budget. Without valid RTP, the
controller uses its fallback cadence/readiness path, with phase and reserve
learning. Do not apply that fallback path's percentile-spread formula to normal
timestamp production behavior.

### 8.3 Cadence smoothing

The **Reduce judder** checkbox controls cadence smoothing
independently of the three VRR timing presets. It defaults on and preserves
existing saved choices. Session startup snapshots it, including across decoder
resets; reconnect after changing it. The stream CLI can override it with
`--vrr-smooth-frame-timing` or `--no-vrr-smooth-frame-timing` without saving.
The label rename preserves the `smoothvrrframetiming` INI key and
`smoothVrrFrameTiming` QML property, so existing enabled and disabled choices
carry over unchanged. Live qualification uses the four-interval gate described
in the cadence qualification correction above; explicit older captures retain
their single-interval/compensating-pair gate.

Unchecked, production follows relative RTP spacing while buffering delivery
variation. Checked, it blends 85% of the predicted source slot with 15% of the raw
mapped slot. This redistributes available waiting time to reduce adjacent
short/long intervals, at the expense of timestamp fidelity. It cannot guarantee
uniform motion between irregularly sampled images or prevent compositor jitter.
RTP values remain unchanged; their arbitrary epoch must not change scheduling.
Conceptually, with `raw = sourceTime + delayBeforeThisFrame` and the learned
readiness reserve `R` (zero for captures without the reserve parameters):

```text
trackedPeriod += 0.025 * (eligibleSourceInterval - trackedPeriod)
predicted      = previousSmoothedBasis + trackedPeriod
error          = predicted - (raw + R)
trackedPeriod -= 0.02 * error                 # phase feedback, next frame
adjustment     = 0.85 * error
adjustment     = clamp(adjustment, -(delayBeforeThisFrame + R), 6000 us - R)
smoothedBasis  = raw + R + adjustment         # cadence_smoothing_us = R + adjustment
```

With the production readiness bound, `adjustment` above is the requested
smoother adjustment. Its clock basis retains that request, while the applied
total adjustment is bounded below by `min(0, readyOffset - delayBeforeThisFrame)`.
Reserve learning also retains the request, so this per-frame bound does not
erase the shortfall the bounded smoothing reserve is intended to cover.

`R` is applied to every timestamp-playout frame while smoothing is enabled,
including frames the smoother cannot currently place, so a cadence reset does
not step the schedule by `R`. It is learned only from frames the smoother
placed: `min(readyOffset - playoutDelay, 0) - adjustment` is the lateness caused
by moving that frame before its raw slot, and `R` tracks its p98 over the last
128 placed frames minus 500 us, capped at 3 ms. See the 2026-09-22 Reduce judder
section above for why and for its evidence.

The actual integer implementation also reseeds from the authoritative fitted
period when necessary and resets smoothing on rebases, rate/phase changes,
untrusted cadence, bursts/stalls, or excessive phase error. This is not a fixed
FPS generator. It follows genuine source-rate changes while attenuating adjacent
short/long timestamp pairs.

Production prediction anchors the next smoother state to the intended target,
not a later actual execution time. Otherwise one late frame would move later
frames and turn a temporary miss into persistent added delay. Older replay modes
retain execution-anchored smoothing and the retired metronome for compatibility.

The 6 ms cap bounds positive retiming including the reserve, not total client
latency. Readiness, queue capacity, timing-preset buffer caps and applicable
presentation floors still constrain the schedule. Smoothing does not add a
queued-frame allowance. Its readiness calibration key gains
`|frame-smoothing=150-25-6000|cadence=2-0|catchup=20|smoothing-reserve=3000-500-980-500|period-feedback=20000`
so profiles from earlier smoothing policies cannot cross-seed it.
All new sessions also append the late-recovery revision and preset buffer
ratio to their calibration identity. Historical traces
retain their recorded parameters and need no schema change.

The initial moderate policy was selected by exploratory replay of the completed
2026-09-10 19:34:42 local capture (76.85 FPS). It reduced submission-interval
jerk above 2 ms from 18.7% to 1.2%, with unchanged mean pacer residence and
0.28 ms more p99 residence. The exact gate failed native-outcome validation;
this is not strict A/B proof or a live visual result. Raw presented jerk and
sender-spacing residual must both be reported: deliberate retiming increases
the latter even when cadence becomes more regular. The deterministic
77 FPS alternating-jitter fixture with clean delivery adds 2.6–3.5 ms mean
delay across presets; spare buffering and workload determine the live cost.

### 8.4 Target, render start, and latch request

The general target construction is:

```text
target = sourceTime + readinessBudget + cadenceSmoothing
       + playoutDelay + renderOffset + presentationSafety
target = max(target, now + renderOffset + presentationSafety)
```

In timestamp production, `readinessBudget` is zero. With prediction enabled,
the nominal render contribution uses typical render work; preparation lead is
a separate scheduling budget. The mapped slot uses the delay in force before
this frame's update. Learning must not retroactively replace its already chosen
source slot with the next delay value.

Implausibly future projections can reseed phase. Production waits for three
qualifying projections, so one early timestamp does not shift the entire stream.
A late frame can clamp to the present execution opportunity while the next
frame retains its own source slot.

Production sets `playout_adaptive_only=0`, `playout_per_frame_latch=1`, and
`playout_rate_protection_enabled=0`. Before applying software spacing floors,
each target is compared with `lastSubmission + displayPeriod + guard`.
This restores vrr14's planned-slot protection rule, without vrr17's extra
225/400 us entry/exit allowance. If it falls earlier and the presenter supports native protection, that slot is latched
and its software floor is disabled. Otherwise the adaptive floor applies.
DXGI uses `Present(1, 0)` for protected slots and
`Present(0, DXGI_PRESENT_ALLOW_TEARING)` for slots that clear that threshold;
`MOONLIGHT_VRR_SYNC_FLIPS=1` synchronizes those too (see 2026-09-28 above). Diagnostic composition already
provides native ordering; its protection capability likewise permits a slot
without the extra CPU floor. It does not expose DXGI tearing flags.

`lastSubmission` in that rule is the spacing anchor, not always the previous
Present call (`latched_flip_anchor=1` in production since 2026-09-22). A
latched present waits in the flip queue until the previous flip plus one
display period, so its anchor is `max(call, previous anchor + displayPeriod)`.
Anchoring to the call let the next tearing present flip inside the panel's
minimum period after a late or compressed frame; capture
`20260922-184032-168` modelled 68 such tears in 91 s at 116/120 (PresentMon on
the 09-21 sandbox capture confirmed the queue model). The worker's final
recheck and replay's recheck mirror use `untornReferenceUs()`: the anchor for
a tearing present, the call for a latched one. Production also latches the
first present whose target is at least `vrr_floor_latch_gap_us=20000` after
the anchor, since the panel may be repeating the previous frame below its VRR
range. Both parameters default to 0, so older captures replay exactly. The
replay's `tear_risks` does not model the flip queue and cannot score this.
Remaining known tear source: AMD present-to-flip latency (PresentMon
msUntilDisplayed p50 1.3 / p99 4.7 ms) can squeeze adaptive pairs spaced just
above the panel minimum.

This allows source-rate changes and recovery from late work without permanently
carrying a refresh-plus-guard delay into every subsequent frame. The explicit
adaptive-only policy remains replayable and takes precedence over latch flags.
Historical rate protection uses the shared below-refresh recommendation cutoff.
Backends without native protection enforce the display-period-plus-guard
software spacing floor. Their presentation mode remains unchanged.

Revision 2 retains vrr17's extra headroom for explicit historical replay;
revision 0 retains the older cadence-based latch policy. At steady
116 FPS / 120 Hz, rounded periods are 8621 and 8333 us with a 100 us base guard:
188 us clears revision 1's interval check but not VRR12's 225 us entry margin.
Historical revision 2 added that allowance on the planned per-frame interval,
without reinstating the 64-frame recovery hold or changing buffer targets.
Neither threshold proves scanout prediction accuracy. Both versions start
their spacing calculation at CPU submission.
The extra margin is a historical safety allowance, not a measured bound on
driver/flip/scanout delay. Native synchronized presents may change actual
latency and cadence even when planned targets are identical. A capture from
the affected machine and Windows visual validation are still required.

Historical revision-2 validation on macOS, 2026-09-12: timing-controller, rate-policy, pacing-worker,
replay-config, DXGI-call-boundary, profile, Vulkan mode-selection and persistent
mode-capability tests pass. The new headroom regressions fail against the
unchanged VRR16 calculation and pass after the correction. Fresh single-frame
and warm-history schema-5 worker fixtures pass exact replay with complete
sequence integrity; all five responsive-buffer stress scenarios pass their
interval and 30 ms p99 latency assertions without saturation. These fixtures
are synthetic. Results are under `build/vrr-hybrid/final-*`; no affected-user
capture was available, no native Windows/Linux gameplay was tested, and no
Windows release or ChaseShare update was produced from this macOS checkout.

Follow-up report: the affected user also sees tearing in Lowest latency and
Balanced well below the refresh ceiling; vrr12 and vrr13 reportedly worked
there, while vrr13 failed higher in the range. The restored 225/400 us margin
does not by itself explain or establish a fix for this broader report.
Comparison of the actual vrr12/vrr13 tags identifies additional differences:

- Both older controllers used 64 clean frames of latch recovery after startup,
  ineligible cadence, source-rate changes or phase discontinuities. Per-frame
  revisions 1 and 2 bypass that recovery state even though it is still counted.
- Both older Windows renderers used `Present(0, 0)` for protected frames. Their
  D3D11 source is identical. Current DXGI protection uses `Present(1, 0)`;
  restoring headroom does not restore the old native queue semantics.
- VRR12 retained its submission-spacing floor for every frame. VRR13 disabled
  it for latched frames while still issuing interval-zero native calls. This is
  a relevant high-rate difference, not proof of the reported failure's cause.
- Older smoothing continued from the late-clamped/floored target. Current
  smoothing continues from the original intended target, allowing quicker
  recovery and potentially shorter following intervals. VRR12/13 also used
  different tail-based buffering and did not have the current latency presets.

Both generations anchor software spacing at CPU submission, not verified
image-change time. A large average source interval therefore does not establish
physical scanout headroom, particularly through stalls or native mode changes.
Distinguish steady-state behavior from recovery and identify the affected native
backend before attributing the report to prediction accuracy. Linux's persistent
presentation modes remain separate from these Windows per-present flags.

Prediction-only production ignores the presentation model's compositor lead
and scanout floor. Deadlines use the mapped source cadence, readiness protection
and measured local work/scheduler budgets. `earliestSubmissionUs()` supplies
the local submission-spacing floor. The existing `predicted_scanout_us` trace
field equals the submission target in this policy; it is not a calibrated
measurement of physical scanout. Historical policies can still learn a
compositor lead and scanout floor from native observations.

Normally that earliest submission is
`lastSubmission + displayPeriod + guard + entrySafetyHeadroom` for revision 2;
historical revisions omit the last term.
With `latchedFloorDisabled` and a latched decision it returns zero. This is a
deliberate reliance on native presentation behavior; it must be checked against
the actual renderer implementation, not inferred from the request flag.

Preparation starts ahead of the target using the existing playout interval plus
learned render/scheduler budgets. Production enables preparation on arrival on
all three platforms and removes the former 6 ms post-submission software delay.
Native acquisition still supplies backpressure when images are unavailable;
deferring the start in software cannot make those images available sooner.
The 3 ms render-lead floor remains subject to source-rate and capacity bounds.
Historical traces retain their explicit preparation and spacing parameters.

## 9. Active production learning and bounded delay

### 9.1 Readiness prediction

`schedule()` retains a pending probe: immutable mapping time, intended source slot,
period, typical render cost, applied delay, guard, and decoder backlog.
Responsive production uses the raw mapped RTP slot for FIFO prediction and
accounts for any smoothing advance separately in the recent estimator. The
historical readiness-driven policy uses the smoothed slot with padding removed.
Preparation and scheduler measurements are recorded for future decisions.
On successful non-cancelled submission, `ReadinessPrediction` models expected
and actual FIFO service using those measurements, excluding intentional pacing
and acquisition waiting from work that should become learned reserve.

The live interval observer also keeps raw preparation, acquisition, explicit
decoder wait, render-scheduler delay, and deferred completion separate. Its
serial-service gate subtracts acquisition from preparation because waiting for a
native image is not throughput that added source buffering can shorten. It then
takes the larger of preparation service and its conservative deferred GPU bound,
then adds the preceding explicit decoder wait and render-scheduler delay. That service gate is
distinct from readiness lateness: readiness explains which interval was delayed,
while serial service decides whether another standing frame could absorb it.

The readiness-history model compares expected progress with actual readiness. Clean samples enter
the reserve immediately. Backlog and work/scheduler/decoder-queue episodes over
a source period are held until recovery. An episode that persists for 2 seconds
or fills the 512-sample holding array is treated as sustained overload rather
than ordinary jitter to absorb with more delay. Once service recovers, the
model can distinguish a recoverable burst from a pipeline that cannot sustain
the stream.

### 9.2 Reserve history and smoothness feedback

The version-18/19 descriptions below are historical. Current production uses
version 20 for five-minute raw FIFO-readiness diagnostics and a separate
`RecentReadiness` estimator for live control (section 9.3). Cached diagnostics
are not a live growth or release gate. Native feedback remains diagnostic.

Linux uses `ReadinessFeedback` and `Reserve(19)`. For consecutive eligible
submissions it compares actual spacing with intended target spacing, then
requires the delayed frame's decode readiness plus preparation/render-wakeup
work to exceed its intended target. Demand is that frame's existing buffer plus
the smaller of spacing error and readiness lateness, minus the 2 ms tolerance.
A catch-up interval uses the earlier delayed frame's buffer, avoiding a second
charge against a newly increased buffer. Native blocking alone, source cadence
variation alone, missing/discontinuous frames, and work or decoder backpressure
exceeding a source period cannot authorize growth. This is observable attribution,
not proof of a counterfactual display outcome; readiness waits include scheduling.

Version 19 rejects predictive version-18 profiles. Its history may retain or
release padding, but only a fresh eligible miss raises requested padding, so a
cached tail cannot drive a new session to the cap. The fixed 3 ms prediction
margin is not added. Existing attack/release limits, source-frame allowance,
and absolute 16 ms cap remain. Historical trace parameters default the new field
to zero. Normal replay selects the Linux rule for a declared Vulkan backend;
exact replay always uses captured parameters. Windows production is unchanged
by this rule.


Windows readiness history uses `Vrr13::Reserve(18)`. Version 18 isolates
prediction-only calibration from native-hitch release floors and older combined
feedback estimates. Readiness history controls both increasing and decreasing
padding; native and CPU submission interval errors do not become buffer demand.
Namespace/file version names do not mean the older algorithm
is active. Reserve uses nanoseconds, 250 us histogram bins, and one-second aging
buckets over approximately five minutes. Allocation is kept out of ordinary
frame observation.

The empirical quantile inside Reserve is p99.95 nearest-rank. All valid samples,
including successes, contribute to the denominator. Versions 15 and 16 consider a
readiness miss when required protection exceeds available protection by at least
3 ms. A recent miss affects trust and temporary boost rather than being silently
diluted by a long good history.

Release can start after a short warmup: at least 32 samples spanning 2 seconds,
with a recent miss blocking release for 2 seconds. That is separate from the
stronger reliability condition involving longer history and at most 0.05%
misses. Five-minute retention does not mean every startup waits five minutes
before adaptation.

There are separate submission and native `SmoothnessFeedback` instances for
diagnostics. Native intervals are compared against `sourceTimeUs`, preserving
relative game cadence while excluding changes in padding, render estimates and
compositor prediction from the desired interval. A measured cadence error must
exceed 3 ms after uncertainty handling to count as a hitch. Source-rate
transitions and host stalls retain the existing eligibility exclusions. These
observations never request or block prediction-only buffer changes. Native
confirmation is OS timing evidence, not optical proof of a perceived hitch.

In the historical native-hitch policy, stretch charges the current frame;
catch-up charges the preceding delayed frame
using that frame's original padding. Each newly confirmed miss supplies demand
once; historical histogram tails cannot repeatedly authorize growth. Missing,
out-of-order, ambiguous, or unmatched native feedback cannot manufacture a miss.
Legacy policies retain their inclusive threshold, original target/scanout
references, and readiness/combined-feedback adaptation for exact replay.
The new parameter defaults to zero when absent from older captures.

`PresentationPrediction` requires explicit display-event timestamps for production
cadence diagnostics and matches them to submitted present IDs. DXGI refresh references remain usable
only by the legacy replay policy; matching their refresh identity does not turn
them into display events. Stale, future, or too-uncertain observations are
ignored. The inspected implementation bounds sample age at 100 ms and native
uncertainty at 500 us, learns a rolling median ready-to-presentation lead, and
uses fresh matched observations for its floor in historical policies only.
Production ignores both the lead and floor. Its `smoothnessProtectionUs`
diagnostic and user-facing smoothness score use submission prediction even when
native events are available. Missing native feedback remains missing in the
separate tracking counters.

### 9.3 Delay update and capacity formulas

`updatePlayoutDelay()` dispatches directly to `updatePlayoutHistory()` when
history is enabled. The later per-rate-band reservoir/percentile branch is
legacy/replay behavior. In that branch 1000 per mille means p100, 999 means
p99.9, and 995 means p99.5. Those values must not be confused with the active
Reserve p99.95 implementation.

Production sets `playout_prediction_only=1` and `playout_responsive_buffer=7`
for every normal VRR session. The interval-quality observer described above owns
requested delay, with 125 us per-frame attack application and preset-timed
release. Historical revisions 9/10 report each interval as
`min(1, max(abs(actualInterval - intendedInterval) - tolerance, 0) / intendedInterval)`
and average that fractional loss over evaluated time in the preset's history
window. Production revision 7 uses
`min(1, max(meanErrorOverOneSecond - tolerance, 0) / intendedInterval)` for
both reporting and buffer control, accumulating time-weighted loss in that
same history. Historical revision 10 uses this mean-based loss only for control,
including hold/release qualification, while reporting per-interval loss. Fresh per-interval
excess and eligible, absorbable late readiness are still required for growth;
the 250 us request step and 250 ms cooldown remain unchanged. Revision 9
retains coupled per-interval control; revisions 7/8 retain their old reports.
With `playout_smoothing_readiness_bound=1`, its readiness attribution
uses the later of the raw and smoothed targets, excluding misses caused solely
by advancing an otherwise on-time frame. The same attribution governs hold
renewal, allowing clean recovery instead of retaining smoothing-induced delay.
It bypasses the following historical percentile growth/release law.
The retired revision-4 estimator keeps 100 ms buckets over the selected preset's learning
window, including successes. Lowest latency uses 99% over 30 seconds, Balanced
99.5% over 60 seconds, and Smoothest 99.95% over 120 seconds. These are
historical best-effort targets within the retired 16 ms latency cap. Historical revisions 1
and 2 retain their three-second p99 estimator for exact replay.
Raw readiness is measured against RTP
source slots in the FIFO model; an earlier smoothed deadline is a separate
cost applied before clamping away early-readiness slack. Revision 1 incorrectly
discarded that slack first, charging reserve even when an advanced deadline was
already covered. In the historical gate, large source changes disable smoothing until 200 ms of eligible
cadence falls within 25% of the fitted period. Revision 2 evaluates compensating
short/long outliers together, so alternating 9/17 ms intervals at a stable 77 FPS
do not keep Reduce judder disabled. A sustained same-direction change still
uses individual intervals and resets confidence. This confidence uses source
time, not a fixed frame count. Actual RTP
spacing remains eligible for delivery learning while the rate fit catches up.

A readiness shortfall over 2 ms renews a two-second burst boost. A shortfall
over 1 ms through 2 ms permits growth only when more than half of the current
readiness window exceeds 1 ms. Shortfalls through 1 ms never grow the buffer.
Only fresh hard misses renew the boost; old histogram or cache tails cannot. The target adds
500 us to the larger of the selected recent percentile and that boost, bounded by the existing
minimum, the live fitted-period preset maximum (or the configured-rate maximum
for historical captures), the 16 ms ceiling, and queue capacity.
Growth is at most 500 us per update. Release requires 32 recent observations,
two seconds of live history, no miss for two seconds, and a sample within
250 ms. It approaches the recent target at approximately 1.2 ms/second, up to
twice that with spare display and processing capacity, at most 250 us per
frame. Thus the falling recent target probes downward without waiting for
five-minute diagnostic tails. Silence/overload is not clean release evidence.
Requested demand remains visible above the cap for capacity diagnostics when
`playout_capacity_telemetry=1`, but is recomputed every frame rather than stored
as debt. No new queue is added. This distinction makes a demand clipped by the
observed-cadence cap measurable instead of making the applied buffer look as if
it absorbed the entire stall.
Revision 3 and later retire live learning history on a confirmed material source-rate
change while preserving the applied buffer for gradual recovery. Host stalls
above max(25 ms, 1.5 source periods) and catch-up intervals below half a source
period do not train the estimator; their playback outcomes still affect the
displayed readiness result. The diagnostic five-minute calibration is unchanged.

Historical prediction-only mode (`playout_responsive_buffer=0`) behaves as follows:

- Required protection is readiness p99.95 plus any recent readiness-miss boost,
  then 3,000 us of headroom. The model includes delivery variation, FIFO render
  work and render scheduler delays, excluding intentional pacing, swapchain
  acquisition waits and post-submission display latency.
- Padding grows toward that requirement by at most 500 us per update. It does
  not wait for a displayed hitch. Existing protection is compared with the
  requirement, rather than added to the next demand. The cold-start seed is
  applied once and is not added again when calculating headroom.
- Padding shrinks toward the same requirement after readiness history warms
  (32 samples and 2 seconds) and any readiness miss has been absent for 2 seconds.
  No display sample is required. The 10 us release input scales by elapsed time
  at a 120 FPS reference, capped at 33,333 us of elapsed recovery per update.
- Readiness tails still age out over the existing five-minute window; a brief
  clean period does not immediately erase a recent burst or valid cached tail.
- Capacity and the selected timing allowance remain hard bounds
  in both directions. Lowest latency allows half a fitted source period,
  Balanced one period, and Smoothest two periods. These bounds can prevent the
  full 3 ms headroom.

The legacy readiness and combined-feedback laws remain available for replay.

Queue capacity is an independent hard bound:

```text
period          = min(fittedSourcePeriod, negotiatedStreamPeriod)
capacity        = 4 * period
occupied        = renderLead + presentationSafety
                + (smoothingEnabled ? maximumSmoothingLag : 0)
queueDelayLimit = max(0, capacity - occupied)
modeAllowance   = fittedSourcePeriod * configuredBufferPerMille / 1000
                # defaults: Smooth 4000, Balanced Target 1000, Low Latency 500
maximumInput    = max(1000 us, configured fitted source-period allowance)
effectiveMin    = min(1000 us, queueDelayLimit, modeAllowance)
effectiveMax    = min(maximumInput, queueDelayLimit, modeAllowance)
```

`maximumSmoothingLag` is `playout_smoothing_max_lag_us` (6 ms in production
since 2026-09-22, 2 ms before), which also contains the smoothing readiness
reserve. Total buffering plus positive retiming stays within the four-period
queue capacity; the bound may be tighter than the fitted-period profile allowance.

The cold start first takes `max(6000 us, 0.95 * sourcePeriod)`, caps that by
`max(displayPeriod, renderLead)` for history mode, then clamps to effective
minimum/maximum. Consequently neither “the buffer always starts at 6 ms” nor
"the maximum is 8 ms" describes current production. The selected source-frame
allowance is further reduced by the four-frame queue-capacity bound. In particular, a four-frame Smooth allowance
does not allocate four waiting frames or guarantee that all four fit. The allowance is not a promise of total
decode-to-submission latency because rendering and applicable native/CPU floors
remain outside the adaptive playout buffer.

More protection can improve jitter tolerance while consuming latency and queue
capacity. If the requested protection exceeds capacity, record the limitation
rather than presenting the capped policy as able to absorb all observed work.

### 9.4 Persisted calibration

`vrr13-calibration.json` lives under the cache path. The profile key includes
display identity, stream FPS, display refresh, smoothing settings,
and session context, including the active native presenter. Balanced Target and
Low Latency add distinct timing-mode suffixes. Enabled smoothing also appends
`|frame-smoothing=150-25-6000|cadence=2-0|catchup=20` and, when the reserve or
period feedback is active, `|smoothing-reserve=3000-500-980-500|period-feedback=20000`
to isolate its readiness history.
Profiles expire after 14 days; saves require at least
240 observations, use locking/atomic replacement, and cap storage at 16 profiles.

Responsive production accepts version-20 raw readiness histograms for diagnostics
only. Startup uses the existing bounded cold-start seed; cached tails cannot
raise or hold the live request. The separate recent estimator starts empty.
Historical prediction-only mode accepts version-18 readiness histograms; versions
from native-hitch and earlier policies are rejected. Its requested protection
uses the readiness distribution and recent boost, not a display-validated
"proven buffer". Reserve ages the prior and replaces its mass
with live evidence over time. Short interrupted runs preserve a more protective
prior instead of automatically erasing it. Cached evidence is not proof of
current-session coverage. Display epoch changes invalidate calibration saving.
Full reset and phase rebase differ: a rebase can preserve learned playout state
while clearing transient timing predictors.

The live interval buffer's initial-calibration flag is not loaded from this
cache. It starts unqualified, requires both 500 ms and 32 consecutive intervals,
and remains complete across subsequent sequence breaks and FPS changes. The
one-second error window, growth cooldown, long quality history and release
holds are independent of this startup shortcut. A broken sequence subsequently
requires a full second before adaptation resumes. The trace captures
`playout_interval_initial_warmup_us` and
`playout_interval_initial_minimum_samples`; missing fields default to the
historical one-second/two-interval behavior, preserving exact replay.

## 10. Windows D3D11 mechanics and native synchronization

### 10.1 Eligibility and GPU synchronization

The renderer prefers the adapter owning the display, creates a flip-discard
HWND swapchain with five buffers, and chooses appropriate RGBA/10-bit format.
It checks tearing support and uses `DXGI_SWAP_CHAIN_FLAG_ALLOW_TEARING` when
supported. Adaptive eligibility checks effective V-sync, borderless fullscreen,
flip-model state, refresh, renderer threading, readiness fencing, tearing
capability/flags, and render/output adapter compatibility. Actual swapchain and
fullscreen descriptors are rechecked through display transitions.

The source deliberately avoids assuming `SetMaximumFrameLatency(1)` is a free
latency improvement: with interval-zero presentation it can make Present block
in a V-sync-like way. Swapchain buffer count is not the worker queue capacity.

With separate decode/render devices, `captureDecodeBoundary()` signals a shared
decode-to-render fence when a decoder output is handed off. Rendering waits for
that exact value, so it need not wait for newer decode work. Render-to-decode
ordering protects texture reuse. The purpose is both correctness and avoiding
accidental waits for work belonging to subsequent frames.
`renderVideo()` queues `ID3D11DeviceContext4::Wait` for the captured boundary
before any copy or shader read; that GPU dependency is the correctness mechanism.
With monitored fences, the worker's `waitForDecode()` also blocks until the
exact captured value completes, taking no context lock and using its own event
with the 50 ms / fence-value-verified wait described below. That post-wait clock
is `decodeCompleteUs`, which production source mapping requires to absorb
hardware decode time (the Linux `vaSyncSurface()` equivalent). Between
`78b99f1c` and 2026-09-22 this check was nonblocking, which left Windows mapped
from decoder output while Linux mapped from completion. Non-monitored fences and
shared devices remain nonblocking; there, a zero CPU decode-wait measurement
does not mean the decode dependency was already complete.

Two mutexes serialize this renderer. FFmpeg's D3D11VA lock (`m_ContextLock`)
guards the decode device's immediate context; FFmpeg holds it for each frame's
entire decode submission. A separate presentation lock guards the render
context, swapchain and prepared VRR frame against window-change callbacks. The
presentation lock includes FFmpeg's lock only when decode and render share one
immediate context. On separate devices, preparation and `Present` never hold
FFmpeg's lock; `renderVideo()` and `captureDecodeBoundary()` take it only
around decode-context `Signal`/`Wait`/`Flush`. Lock order is presentation, then
context.

Preparation binds and clears the backbuffer, renders video and overlays, and sets
colorspace/HDR state. Direct decoder texture binding follows stock Moonlight on
Intel and on separate decode/render devices. AMD/NVIDIA single-device sessions
keep the compatibility copy below 4K; 4K streams bind when the GPU has Feature
Level 11.1+ or D3D11 fences. It holds the presentation lock while manipulating
render state. Immediately after recording the frame, preparation signals and
flushes a present-ready fence, arms its event, and performs one nonblocking value
poll. It publishes the prepared frame without waiting for completion, reports
`sourceFrameReusable=false`, and releases the presentation lock before the
worker's cadence hold. The source `AVFrame` remains owned through presentation;
on the separate-device path, the render-to-decode fence also protects decoder-
surface reuse.

At the target boundary `presentAdaptive()` reacquires the presentation lock and
verifies that exact present-ready value before calling `Present`. It releases
that lock while waiting (on a shared device this also lets decode continue), then
reacquires and revalidates the prepared frame, display state and device. Usually the cadence hold
has already covered the GPU work and this is a completed-value poll. A late frame
pays only the residual wait at the target. The complete fence wait has a 50 ms
bound, blocks on the event in 1 ms slices, and checks the fence value between
waits; the value, rather than notification delivery alone, proves readiness. A
stale notification cannot release incomplete work, and a delayed notification
cannot hold already-completed work for 50 ms. Display/resize mutation drains an
outstanding prepared fence before invalidating its backbuffer. The device-removed
sentinel is rejected. A true fence timeout or wait failure disables the adaptive
path and requests recovery; its log includes the target, final completed value,
last event result and device status.
Decode-to-render and render-to-decode Signal/Wait failures likewise abort frame
preparation and request device recovery; an unsynchronized frame is never
submitted as a fallback.
`gpu_ready_wait_result` records the aggregate fence-wait status in Win32 wait
codes; individual slice timeouts are not whole-fence failures. The trace's poll
fields describe the first poll at the final Present-boundary check, replacing
the nonblocking preparation poll when that check runs. A cancelled frame before
the final check can still carry the preparation poll. Poll and final readiness
timestamps retain conservative completion bounds. Historical captures retain
their original single-event-wait observations unchanged.

When the target-boundary check completes successfully, the VRR worker records its
residual verified wait as `gpu_readiness_applied_us` and the next controller
decision exposes the bounded `gpu_readiness_lead_us`. The estimator uses only completed waits in
the current ten-second window, takes the configured percentile (99% in live
sessions), adds a 500 us margin, and slews toward that demand. It is a render
start opportunity, not a promise of completion: a late fence still follows the
existing failure/recovery and presentation path, and the current source target
is never moved to hide the stall.

Fence completion proves the prepared backbuffer work has finished before Present.
If the recorded poll found the value complete, its end is the completion upper
bound; otherwise the successful final wait is the upper bound. A final-boundary
poll may overstate service because the fence could have completed earlier during
the cadence hold. Replay accepts either a preparation poll or a final poll and
anchors counterfactual upper-bound mapping according to that poll's placement.
The controller uses the resulting uncertainty conservatively for the serial-
service gate. CPU poll/event brackets are not an exact hardware timestamp or a
measurement of total GPU execution time.

### 10.2 Native Present parameters and telemetry

On the DXGI path, `D3D11VARenderer::presentAdaptive()` creates one
`DxgiPresentParameters` value from the controller's latch request. The same value supplies native
telemetry and `presentPreparedFrame()`, which forwards it to DXGI:

- Latched: `Present(1, 0)`.
- Adaptive: `Present(0, DXGI_PRESENT_ALLOW_TEARING)`.
- Legacy: interval zero with the existing `legacyPresentFlags()` value.

The controller can omit its software spacing floor for a latched decision;
passing interval one to DXGI is therefore part of the renderer contract.
`tst_dxgipresent` exercises the actual shared call boundary using a fake
swapchain, including transitions, parameter reporting, and native result
propagation. Actual display behavior still requires Windows validation.

Historical builds computed and recorded interval one but hardcoded zero in
the native helper. Their `latched_present` and native interval fields describe
intent, not proof that DXGI received interval one. Replay cannot repair that
old instrumentation or turn historical `confirmed_safe_latched` classifications
into independent scanout evidence. Check the executable used for each capture.

`restoreFixedPresentation()` disables VRR and retains the swapchain. The legacy
software-paced caller still explicitly uses interval zero; this correction does
not change its pacing mechanism.

### 10.3 Native evidence limits

Only `S_OK` is treated as a presented result. Failed calls request recovery;
non-display success statuses such as occlusion are cancellation outcomes rather
than proof of monitor delivery.

`GetLastPresentCount()` and `GetFrameStatistics()` can report earlier operations.
`PresentRefreshCount` and `SyncRefreshCount` are different identities;
`SyncQPCTime` timestamps the sync observation and is not automatically the
presentation timestamp of the accompanying present ID.

Production cadence diagnostics require `playout_require_display_events=1`. Presentation feedback
explicitly distinguishes unavailable timestamps, refresh references, and display
events. DXGI `GetFrameStatistics()` is marked as a refresh reference and cannot
teach compositor latency, authorize buffer growth/release, or create a measured
cadence sample, even when its refresh IDs match and its timestamp follows
submission. The Windows-host vrr14/vrr15 captures and the Ally GTA capture
demonstrate why: different present IDs can share an identical raw `SyncQPCTime`,
including frames submitted after that reference. A reference is not the frame's
image-change instant. The old inference remains available only through explicit
historical replay parameters; missing `playout_require_display_events` defaults
to zero to preserve old exact baselines. New schema-5 traces additionally record
`latch_time_kind` (0 unavailable, 1 refresh reference, 2 display event).

The DXGI fallback statistics provider supplies no verified display events.
Production uses submission estimates for its client cadence report; independent-
flip events from the preferred composition presenter populate the separate
display graph and present-timing diagnostics.
Readiness prediction independently adapts padding in both directions. Composition-frame statistics also lack a verified frame
display instant, so the same estimator covers periods without independent-flip
events. This lower-confidence timing remains internal telemetry; it does not
claim native display coverage or learn display-service latency from it.
Linux Wayland presentation feedback and Gamescope actual-present timestamps
are explicitly marked as display events and remain eligible for measurement.

`MOONLIGHT_VRR_ALIGN=1` enables observation-only raster probes around Present.
DisplayConfig signal geometry and QPC correlation support phase modeling.
`D3DKMTGetScanLine()` reports raster position around a CPU observation; it does
not establish when a queued flip became visible. Cloned/ambiguous display paths
must not be silently treated as an exact calibration match.

Software timing, tearing permission, and modeled active-scanout exposure do not
confirm an optical tear or its absence. External display measurement is needed
for that claim.

### 10.4 Composition presentation and display timing

Eligible Windows VRR sessions attempt the composition presenter by default.
`MOONLIGHT_VRR_COMPOSITION=0` disables the attempt; `1` retains its previous
explicit opt-in behavior. The value is captured during renderer initialization,
so a stream reconnect is required. Startup logs identify the actual presenter,
including setup fallback. Hardware support permits independent flip but does
not prove that every live present uses it.

The renderer checks the actual OS version using `RtlGetVersion` (including
revision 194 on build 22000), loads `CreatePresentationFactory` dynamically,
and requires `IsPresentationSupportedWithIndependentFlip()`. OS version alone
is insufficient. The render device uses BGRA support and disables internal
threading optimizations as required by this API; an unsupported device retries
with the original DXGI device flags. Setup failure retains the DXGI path.
System-relative presentation time is QPC scaled to 100 ns with the actual QPC
frequency. It no longer depends on resolving an interrupt-clock export or assumes
that the interrupt-clock epoch equals the presentation clock's epoch.

`D3D11CompositionPresenter` owns five displayable textures, a presentation
manager/surface, and a DirectComposition visual bound to the streaming window.
Initial allocation and each resize explicitly set the surface source rectangle
to the full buffer; omitting this leaves successful submissions with no image.
The same shaders, overlays, colorspace, GPU-ready fence, and pacing deadline
are used. Buffer acquisition checks availability without waiting. Submission
cancels older pending presents and targets the current QPC-derived presentation time, with
no added source period or wait for a presentation event. `ForceVSyncInterrupt`
requests prompt statistics even with hardware flip queues. Buffer storage
is not a queue-depth target; OS/driver scheduling still needs measurement.

Only independent-flip statistics with the matching surface tag, output adapter,
source ID, and increasing present ID become display events. Their 100 ns system-relative
timestamps are correlated with scaled QPC through a fresh bracket on the worker
clock, with bounded age and uncertainty. Composition statistics do not become
display events. The backend is trace value 3; DXGI flags, query results, and raw
QPC fields remain unset. The explicit `native_synchronized_presentation` flag
keeps every controller decision latched and omits the software floor without
claiming DXGI flag switching. Missing flags default to zero for historical exact
replay; session-policy replay resolves the current composition capability.
Resize replaces
buffers; display changes recreate the renderer and its output identity.

`compositionprobe --run` is an optional fullscreen Windows hardware diagnostic
for independent-flip coverage and submission-to-display latency. It does not
prove optical VRR, tear freedom, or end-to-end latency. The 2026-10-04 native
ALLYTWO probe confirmed independent-flip coverage with no rejected timestamps
or unavailable buffers. At 116 FPS/120 Hz, steady-state coverage was 464/465,
submission-to-display p50 was 7.057 ms and p99 was 15.968 ms. Its strict
below-one-refresh p99 gate failed (exit 2); native capability does not guarantee
sub-refresh latency or live VRR cadence. The 60 FPS probe also failed that gate.
These are synthetic presenter observations, not gameplay or optical validation.

Windows integration validation for this update: the incremental application
build and diagnostics build pass. All six required VRR suites, DXGI call-boundary
and presentation-clock suites pass. The overlay check passes on retry after a
stats-only asynchronous assertion in the unchanged fixture. The new composition
worker fixture populates 22 matched display intervals for 24 submissions and
passes schema-5 exact replay, as does the existing worker fixture. The newest
completed capture in the canonical trace roots (`20260926-131302-979`) remains
exploratory: old and new replay both exit 3 with identical fidelity results
(6,622 exact targets and tear classifications, 6,621 exact submissions). This
change has not been validated in a new live gameplay stream.

## 11. Other presentation paths

The shared presenter interface separates support checks, decode-boundary
capture/readiness, preparation, adaptive presentation, cancellation, and feedback.
Its implementations can have different acquisition and cancellation semantics.
Completion feedback may be available during preparation, deferred until the
native-present boundary, or unavailable when the backend instead retains source
ownership asynchronously. The worker treats those cases explicitly.
Do not transfer D3D11 fence or Present assumptions directly to Vulkan.

### macOS Metal presentation

An Auto VideoToolbox renderer request uses `VTMetalRenderer` when either VRR
playback or the matching startup renderer probe is requested. The probe does
not start adaptive presentation; it keeps color-range/HDR negotiation aligned
with the playback renderer. The native display helper qualifies both continuous
Adaptive-Sync ranges and discrete ProMotion ranges on macOS 12 or newer. A
requested checkbox, the 120 Hz maximum, or the existence of a Metal device
alone cannot establish a variable-refresh range. Playback also requires native
Cocoa fullscreen; a windowed or older borderless path falls back to fixed pacing.

Active Metal VRR uses a persistent `CAMetalLayer` with
`displaySyncEnabled=YES`, three drawable slots, and drawable acquisition timeout
enabled. `CAMetalDisplayLink` is bypassed only while the shared VRR worker owns
cadence. A synchronized layer can retain both its displayed drawable and a
submitted successor; the third resource lets early preparation proceed without
waiting for the compositor to retire either. The worker still prepares one
image at a time and submits at its existing target, with no additional playout
queue. Startup logging records drawable dimensions, bounds, count, and
transaction mode alongside the native display range.
Preparation acquires a drawable, encodes the existing video/color
conversion and overlays, commits the render command buffer, then observes its
completion with a 50 ms CPU wait bound. Acquisition is a separately measured
native backpressure cost; that 50 ms bound applies to command completion, not
the entire preparation call. Failed rendering or a timed-out wait requests
renderer recovery rather than submitting an incomplete image.

Each render command retains its source `AVFrame` and any CoreVideo texture-cache
wrappers until its GPU completion callback. Those references remain valid even
if the bounded wait fails and the worker/renderer is torn down. Successful
completion permits the worker to release its source before the target hold;
the prepared drawable remains owned until presentation or cancellation.
Cancellation releases an already-rendered drawable without a native submit.

At the worker's target, `presentAdaptive()` calls
`presentAfterMinimumDuration:` with the screen's native minimum refresh
interval. This constrains the previous drawable's visible duration; it is not
an added sleep or source frame period. Every VRR drawable is synchronized, so
Metal advertises native protected-slot support without switching modes for the
controller's per-frame latch request. On fixed fallback the layer returns to the
session's V-sync setting and three drawable slots, and the existing fixed
presentation/display-link path remains available. An active display change
requires renderer recreation even if both screens have the same maximum Hz,
because the native interval range and drawable epoch can differ.
Readiness calibration identity snapshots the Metal device registry ID, stable
CoreGraphics display UUID, and native minimum/maximum/granularity during main-
thread initialization. The worker therefore does not touch AppKit while using
or saving calibration history.

Metal is native backend value 4. Submission IDs are local increasing serials.
The native method returns void, so result 0 denotes accepted submission, not
proof the compositor showed the drawable. DXGI query results, tearing flags,
adapter snapshots, fence values and raw QPC fields remain unavailable. GPU-ready
fields describe the CPU-observed command-completion bracket, not an exact GPU
execution timestamp.

`addPresentedHandler:` obtains the drawable's OS-reported `presentedTime`.
A nonpositive time supplies no event. A fresh `CACurrentMediaTime()` reference
is sampled inside a `LiGetMicroseconds()` bracket; the shared clock translator
rejects future events, events older than 100 ms, and brackets wider than 500 us.
Callbacks retain only shared observation state, never a renderer pointer or
decoder surface. A later submit can report a previous serial's `DisplayEvent`
with correlation uncertainty. Shared diagnostics match that identity and keep
missing events as gaps; requested latch transitions do not reset Metal's fixed
native presentation mode. This is compositor evidence, not optical confirmation
of panel cadence, tearing, or end-to-end latency.

First external-display investigation (2026-10-04): clean-close capture
`20261004-222950-425-c89ef903-1755-412c-bf82-cf0abaaea792/Moonlight.vrrtrace`
(SHA-256 `415d1cb8f8914cafa4c3b05d4ebc7d784fd0289dc4babe1d3d76312716baee19`)
requested 4K90 PyroWave 4:4:4 10-bit on the LG TV's advertised 40-120 Hz range.
The log reported 90.19 incoming FPS, 71.02 rendered FPS, and 21.05% client pacing
drops. Recorded drawable acquisition averaged 9.01 ms, versus 1.45 ms rendering;
native display intervals were predominantly 16.67 ms. Trace sequence/footer
integrity passes and all controller targets and submissions replay exactly,
but five queue-depth semantic errors reject the strict baseline gate. This is
recorded execution evidence, not strict counterfactual A/B proof.

A host-free 600-frame native smoke at 90 FPS on the same display reproduced
401 submissions and 199 drops with two resources. Three resources produced
600 submissions and zero drops and passed exact replay. The observed display
intervals still alternated around 8.33/16.67 ms; successful submission throughput
must not be described as adaptive display-cadence success. The constant-rate
`check_metal_cadence.py` hardware gate independently checks matched OS display
events and rejects that result. The user confirmed that macOS and the TV are
already in VRR mode; asking to enable VRR again does not resolve this failure.

Follow-up native experiments explicitly requested 90 FPS through a
`CADisplayLink`, and separately passed the controller's fitted 11.111 ms source
period to `presentAfterMinimumDuration:`. Neither produced uniform 90 Hz OS
presentation events. The fitted-period run submitted all 600 frames, matched
99.65% of steady display events, but only 0.35% of consecutive intervals were
within 500 us of the requested period; acquisition rose to 4.63 ms on average.
Both experiments were reverted. A standalone Cocoa/Metal control using
`CAMetalDisplayLink`, a 90 FPS preferred range, and its supplied drawables also
received predominantly 120 Hz callbacks. A display-link preference is a
best-effort request, not proof of adaptive scanout, and its drawables reject
`presentAfterMinimumDuration:`; mixing the two APIs throws a native exception.
These experiments do not establish the root cause or justify a new production
timing policy. The remaining investigation is native presentation eligibility,
including direct versus composited presentation; optical panel validation also
remains outstanding. The resource correction must not be presented as a VRR fix.

Continuation on 2026-10-04 separated environmental throttling from native cadence.
The user observed the TV's numerical refresh rate vary toward 90 Hz in the native
control. The Metal HUD then exposed Low Power Mode, composited presentation, and
about 60 presented FPS during a 90 FPS request; the control's display events were
predominantly 16.67 ms. The user changed AC power mode to Automatic. A fullscreen
settle and a focus check were also necessary: an earlier unsettled smoke dropped
245 of 900 frames and cannot establish steady fullscreen performance.

With Automatic power, fullscreen settled, and no focus loss, the production
presenter submitted all 900 frames with zero drops and passed exact replay.
Recorded display intervals were distributed around 11.11 ms rather than a fixed
120 Hz grid. The strict constant-cadence gate still failed: 44.71% of consecutive
events were within 500 us, with 2.111 ms p95 interval error. Passing the fitted
source period to Metal also submitted all 900 frames but yielded similar cadence
(47.97%, 2.249 ms p95); that experiment was reverted. Neither gate failure alone
proves VRR is inactive, nor does zero-drop throughput prove uniform scanout.
Optical validation and an end-to-end stream remain separate from this smoke.

Initialization now logs Low Power Mode and warns when it is enabled. The advertised
`NSScreen` range remains a capability snapshot, not proof that the current power
policy permits the requested presentation rate. The native smoke logs fullscreen,
activation, focus, and power state, waits for fullscreen to settle before the
worker run, and rejects focus loss during frame delivery.

### Linux Vulkan presentation

On Linux the VRR request prefers the Vulkan frontend. The adaptive mode is
selected for the surface at startup: Mailbox on ordinary Wayland, Immediate on
X11/KMSDRM, and Immediate on Gamescope. Gamescope additionally tries Mailbox
when the dormant SteamOS experiment is enabled, according to exposed surface
capabilities.

The startup decoder probe that asks the host for a color range must use that
same Linux Vulkan preference without activating VRR presentation.
`chooseDecoder()` still forbids `enableVrr` in test-only mode; the probe passes
`preferVrrRenderer` from the session VRR snapshot instead. Otherwise an EGL
probe can request limited-range video while VRR playback interprets it as full
range, which washes out SDR. Vulkan's AMF AV1 full-range mapping override
applies only when the negotiated stream range is full. HDR remains gated on the
client's Enable HDR preference; VRR does not advertise 10-bit formats by itself.

For VAAPI hardware frames, the worker performs the explicit `vaSyncSurface()`
readiness check once and records its CPU wait. Preparation no longer repeats that
explicit synchronization; libplacebo's `AV_HWFRAME_MAP_READ` import still
validates/maps the dependency. A failed explicit VA sync disables the adaptive
path and requests renderer recovery before preparation can import or read the
surface. This removes redundant CPU serialization without treating decoder
output as proof of GPU completion.

Hardware Vulkan preparation retains the mapped `pl_frame`, including libplacebo's
AVFrame reference and imported source textures, until their GPU reads finish.
Retained hardware output is submitted asynchronously in every presentation mode:
libplacebo transitions the output image, signals a render-complete semaphore, and supplies it to
`vkQueuePresentKHR`. CPU completion is not required for this handoff. Preparation,
presentation and cancellation retire idle source mappings and preparation reports
`sourceFrameReusable=true` only when no retained mapping remains. This releases
the worker's surface before the target hold without recycling external decoder
memory while Vulkan reads it. Cancellation and failure can leave mappings pending;
they remain owned until idle or healthy-GPU teardown finishes them.

The first asynchronous output-wait bypass was withdrawn after the 2026-09-19 23:54:19
clean capture: 117.67 incoming FPS, 85.29 rendered FPS, 27.52% client drops and
zero network drops. Concurrent Vibeshine capture was reported to cause a hard
stall. Restoring the barrier did not establish the cause. The subsequent trace
proved decode-wait discard starvation, corrected in `78b99f1c`. The 00:30:01
launch then produced 76.11% client timing / 100.23 rendered FPS at 301.5 Mbps,
versus 99.67% / 117.37 FPS at 57 Mbps, both Smooth without concurrent capture.
After 20 seconds from the first arrival, high-bitrate presented frames averaged 9.559 ms of serial
decode-wait + preparation + submission service against an 8.333 ms period.

The updated path pairs asynchronous retained-hardware output and the corrected stale
policy with preparation on arrival. It retains up to four source mappings to
prevent capacity stalls, applies bounded retirement backpressure before acquiring another swapchain image, and
never calls an unavailable output-completion sample a zero-duration completion.
Immediate presentation omits that CPU completion check
when the source mapping is retained. GPU-delayed flips may still bunch despite
correctly spaced CPU submissions. Software and unretained imports keep the 50 ms / 100,000-observation output-poll bound.
D3D11 fence/event fields remain unset on Vulkan rows. Windows explicitly flushes the decoder context
after signaling its cross-device boundary, so dispatch does not depend on a later input frame.

The selected adaptive mode remains immutable for the lifetime of one persistent
swapchain. Per-frame controller requests never destroy or recreate that chain.
Persistent Mailbox provides synchronized, stale-image-replacing presentation,
so it advertises protected latch support without a native mode change or the
controller's redundant software spacing floor. Immediate retains the
display-period-plus-guard floor because it may tear. Explicit historical
revision 2 adds a 225 us entry margin to that floor; current revision 1 does not.
That historical extra spacing can reduce sustainable throughput near native refresh;
worker stale-frame replacement remains responsible for bounded backlog.
It does not turn Immediate into a tear-free native presentation mode.
A FIFO-only compatibility path likewise does not advertise
adaptive latch support because it may accumulate queued frames. Actual resize,
reset, or fallback can recreate the swapchain and restores the cached
colorspace/HDR hint before the next acquisition. Deterministic tests do not
establish compositor or physical scanout behavior.

Gamescope WSI's FIFO compatibility exception is used when Immediate is unavailable
and the Mailbox experiment is disabled or Mailbox is unavailable. Although the WSI layer sends Mailbox to the underlying
driver, it forwards the application's original present mode to Gamescope, which
implements FIFO commit scheduling itself. Selecting Mailbox explicitly avoids
that FIFO policy. Steam's frame
limiter can still override a request to FIFO. Native presentation here remains
compositor-owned, and submission success is not physical scanout feedback.
See [SteamOS VRR investigation](docs/steamos-vrr.md) for source evidence and the
reversible composition test for performance-overlay-dependent stutter.
Unsupported renderer combinations
fall back to fixed pacing. Windows Vulkan remains rejected for VRR, and macOS
VRR uses the native Metal path above rather than Vulkan swapchain mode switching.
DRM VRR properties, compositor policy,
and physical scanout evidence remain separate from the client's mode request.

Gamescope timing rejection counters are cumulative across swapchain resets and
logged at renderer teardown. They distinguish empty queries, returned and emitted
records, unmatched IDs, timestamps before submission or in the future, stale or
invalid timestamps, clock-correlation failures, and skipped warmup. They do not
relax validation or change pacing. Sparse usable feedback alone does not establish
that the compositor dropped the other frames. The installed Gamescope 3.16.23.5
source supplies its scheduled vblank target as `actualPresentTime`; these records
must not be treated as an independent physical scanout measurement.

The worker presents only newly received frames and waits for queue activity
when empty. It no longer retains and re-presents the last image to fill gaps.
Display-side low-frame-rate compensation remains the display's responsibility.
Historical `gap_fills_before` and `gap_fill_last_us` CSV columns remain reserved
and zero-valued to preserve trace compatibility.

## 12. Audio, input, and end-to-end latency

Audio has its own UDP/RTP queue, Opus decoding callback, and renderer/device
queue. See [AudioStream.c](moonlight-common-c/moonlight-common-c/src/AudioStream.c),
[audio.cpp](app/streaming/audio/audio.cpp), and
[sdlaud.cpp](app/streaming/audio/renderers/sdlaud.cpp).
Audio packet duration controls its sample cadence, independent of video FPS.

The SDL renderer requests at least 480 samples (10 ms) or three Opus frames,
providing buffering for audio jitter. It uses pending-audio and device-queue
limits (including 30 ms and 50 ms checks) rather than video timestamps to control
backpressure. Audio startup intentionally discards an initial backlog of about
500 ms; renderer reinitialization similarly prevents downtime becoming permanent
queued audio latency. Muting can suppress audio processing without retiming VRR.

Input goes from SDL handlers to common-library input APIs and a separate sender.

Windows/Linux DualSense waveform feedback (updated 2026-10-05) runs separately
from video and ordinary stream audio. A Bluetooth Sony DualSense/Edge with an
exact SDL HID device path can advertise controller capability
`LI_CCAP_HAPTICS_PCM` (`0x8000`) after its output backend opens successfully.
The client also advertises `ML_FF_HAPTICS_PCM` (`0x04`) in SDP. Vibeshine captures
48 kHz S16LE actuator channels 3/4 from its virtual USB audio interface and sends
5 ms stereo blocks using encrypted control type `0x5601`, unreliable ENet
channel `0x08`. Both flags and the versioned payload are a coordinated extension
in these forks; they are not an upstream Moonlight protocol guarantee.

The session registers the waveform receive callback on both Windows and Linux
with SDL 2.24 or newer. The common library advertises the SDP flag only when
that callback is present; enabling the controller backend alone cannot enable
waveform delivery. A Linux-only session callback guard previously left Windows
controllers advertising `LI_CCAP_HAPTICS_PCM` while the connection omitted
`ML_FF_HAPTICS_PCM`, so the host discarded their PCM feedback.

The receive callback validates exact length, version, format, controller range,
reserved fields and sample count before copying into a bounded per-controller
queue. It never accesses Session/InputHandler objects that may be tearing down.
The shared worker resamples to 3 kHz signed 8-bit stereo with SDL's audio stream,
then sends SAxense-derived Bluetooth reports. Linux uses the controller's hidraw
node and verifies its kernel bus identity. Windows opens SDL's exact device
path with shared access, verifies Sony VID/PID and the Bluetooth gamepad HID
collection's report lengths, and writes through overlapped Windows HID I/O.
Output is padded to the descriptor's maximum report length while preserving the
142-byte waveform report's CRC offset. A pending write has a 40 ms completion
wait; timeout cancels and drains the operation before its buffer can be reused
or freed. This is not a hard bound on a faulty driver's cancellation completion.
Input stays with SDL; no kernel module or Bluetooth reconfiguration is added on
the client. Windows USB additionally matches the exact SDL Sony USB controller's
HID device container to its active four-channel WASAPI render endpoint. The
worker submits 48 kHz float samples to actuator channels 3/4 while keeping
channels 1/2 silent; endpoints whose mix format is not four channels are rejected.
Shared-mode conversion uses the endpoint's channel mask. Startup buffers 10 ms
(or waits at most 10 ms); the software FIFO is capped at 40 ms, separately from
the requested 20 ms engine buffer. The actual engine buffer size is queried.
Idle output drains to silence, and disconnect joins the worker before SDL closes
the controller. Linux USB and other platforms keep ordinary rumble. Native game
haptics must originate from the host's controller audio endpoint. Linux uses
its composite DS5; Windows uses the optional local usbip-win2 composite DS5,
selected per application with `dualsense-haptics` or globally with `usbip_ds5`.
The wired host identity is independent of the physical client's USB/Bluetooth
connection. USB/IP stays on the Windows host; no USB/IP transport is required
on the Moonlight client. Game soundtrack audio is not a substitute.

Bluetooth playback drops old/duplicate packets, resets conversion history on
packet loss,
bounds its input and converted queues, sends silence on underflow/idle/removal,
and joins its worker before SDL closes the controller. It cancels SDL emulated
rumble when waveform playback starts and suppresses legacy rumble while active;
LED, motion and adaptive-trigger callbacks retain their own paths. Shared
receive admission rejects late/duplicate sequences, including wrap, before
changing rumble priority; the sequence remains tracked across idle silence.
A silent PCM stream cannot start playback. Both workers idle 60 ms after the
last nonzero actuator sample, send/drain final silence, and stay inactive while
an open host audio endpoint continues to submit zeros. This prevents silent
audio from indefinitely suppressing rumble or overwriting it with silent
Bluetooth reports. Windows HID transport includes the SDK before SDL so
`CancelIoEx` remains declared when compiled separately. Write failure
stops that waveform worker and logs the need to reconnect; ordinary controller
input continues. Hardware coexistence with other applications writing the same
controller still requires physical testing. The USB backend has not been validated
on Windows hardware; Linux deterministic tests cover the shared input queue and
existing Bluetooth worker, not WASAPI endpoint discovery or physical output.

Adaptive triggers already use `SDL_GameControllerSendEffect` on Windows and
Linux, independently of PCM support. SDL owns Bluetooth framing/CRC. The shared
47-byte effect builder sets only trigger validity bits, preserving waveform,
rumble, LED and audio state. Callback admission and event consumption reject
controller indices outside 0–15; allocation and event-push failure release the
report safely. Failed SDL effect submission is logged. Trigger dispatch runs on
the input thread so controller removal cannot race its SDL handle.

The pinned SAxense source, license, research credit and adaptation notes are in
[`third-party/saxense`](third-party/saxense/PROVENANCE.md). Every binary embeds the
original/adapted covered source and both license texts, printable headlessly with
`--haptics-license`. Hardware-free validation is `tests/haptics/haptics.pro`;
passing it does not establish actual Bluetooth/game behavior. The suite runs on
Windows, Linux and macOS using a recording output and SDL's real resampler; a
virtual controller checks trigger payload dispatch. Windows CI builds the native
HID transport and runs the suite. Local 2026-09-19 validation passed on macOS
using isolated headers from the pinned common-library revision; Windows native
build, simultaneous physical waveform/trigger playback and deployment remain
unverified. The pre-existing local common-library checkout was left unchanged.

See [InputStream.c](moonlight-common-c/moonlight-common-c/src/InputStream.c) and
[input handlers](app/streaming/input). Mouse movement is coalesced/batched with
a 1 ms interval; the stream event loop normally sleeps 1 ms when idle, with
platform differences. Input does not wait for the next video target to be sent.

The inspected paths show no client mechanism that makes audio playout follow
VRR targets or makes VRR follow the audio device clock. Added video protection
therefore must not be assumed to produce a corresponding audio delay.

An end-to-end interaction contains client input collection/sending, host
simulation, host capture/encode, network transit, client receive/decode,
presentation, and physical scanout. Client statistics expose only portions.
Host-processing reports, local decoder/pacer timings, and RTT cannot be summed
into a precise physical latency measurement without defining non-overlapping
boundaries and obtaining the missing evidence.

## 13. Trace architecture and replay fidelity

### 13.1 Capture mechanics

`MOONLIGHT_VRR_TRACE` enables worker tracing. Local `.vrrtrace` output begins
with `MLVRR1\n` and stores independently compressed CSV chunks. A `.csv` path
selects CSV output. UNC capture paths are rejected to keep network I/O away
from frame delivery. `MOONLIGHT_VRR_DEEP_TRACE` requests deeper instrumentation;
alignment is the separate native raster option described above.

The Settings recording wrapper creates a unique per-stream folder under
`Desktop/vrr-diagnostics`, sets only the trace/deep-trace variables, and collects
the existing redacted session logging plus a small configuration manifest.
Export is an asynchronous, atomic ZIP containing only this capture's known files.
Active recordings are locked against export; previous environment values are
restored at stream cleanup. This shared Qt code is used on Windows, Linux, and macOS,
with wide Windows file paths and rejection of mapped-network capture roots.

The writer consumes a bounded 8192-row MPSC ring. Producers reserve and publish
slots with atomic sequence numbers; the writer never owns a producer mutex.
A full queue or exhaustion of 16 bounded reservation attempts drops diagnostic
rows rather than blocking presentation. The background writer polls every 2 ms
when empty; formatting, compression and file I/O remain off the pacing thread.
The size policy uses a 512 MiB cap only after at least an hour of arrival-time
coverage. Clean-close footer accounting, row sequences, dropped rows, write
failures, and cap state therefore matter to replay fidelity.

External launchers choose one canonical trace path per application run; the
Settings wrapper chooses one per stream. Before a new
worker reuses an existing file at that path, it archives the previous connection
as `<base>-connection-1.<suffix>` (then 2, 3, etc., without overwriting an existing
archive). If archiving fails, tracing is disabled instead of destroying the old
capture. The canonical path and launchers' latest-trace links continue to refer
to the current connection. Each archived file keeps its own schema and footer.

Rows carry frame identity, receive/assembly/decode times, queue lifecycle,
controller decisions and resolved parameters, preparation/wait/submission
timings, native results and IDs, GPU readiness bounds, and optional deep/raster
evidence. Schema-5 decision rows now include `gpu_readiness_lead_us`; outcome
diagnostics include `gpu_readiness_applied_us` when the deferred D3D11 target-
boundary fence check or a synchronous Linux output poll measured
a completed wait. Linux hardware source-retirement polls do not populate this
field. Terminal rows may be emitted outside the controller-owning worker
and intentionally lack its live diagnostic state.

The optional schema-5 diagnostic extension records `decoder_output_us` separately
from `decode_complete_us`. The former is immutable decoder output and the latency
origin; production currently selects decode-completion mapping. When an explicit
worker wait is material, the latter is
advanced to the post-wait clock as a conservative completion observation; it is
not formed by adding a residual wait to decoder output. Older policies may retain
their captured construction and use it for source mapping. Overlay client processing is
`present_end_us - decoder_output_us`, and queue/pacing subtracts `prepare_us` and
`present_call_us` and the explicit `decode_sync_wait_us` from that same interval. Older traces cannot reconstruct this
boundary exactly; readiness-to-submission is not an interchangeable latency metric.

Every row also records `session_latency_mode`, `session_readiness_hitch_feedback`,
`calibration_loaded`, `initial_cached_samples`, and `history_version`. The
`session_allow_tearing` column records the native permission. New sessions
always record it as enabled; historical captures without it default to enabled,
while replay retains an explicit false value from an older capture. Worker
decision rows have `history_state_valid=1` and scalar snapshots of history samples,
misses, duration, and release eligibility. These snapshots are taken at trace
enqueue, after the outcome, rather than at the earlier scheduling decision.
Nondecision rows have invalid/zero history snapshots. Cached sample count is the
startup prior, not current live evidence. No additional histogram calculation,
formatting, or I/O occurs on frame delivery; these additions do not steer policy.
The existing initial-profile column identifies the complete calibration snapshot.
Optional buffer-accounting fields also include `buffer_calibration_complete`,
`buffer_calibration_samples`, and `buffer_calibration_coverage_us`. They describe
live interval qualification, separately from cached readiness history. Replay
audits their values when present; missing historical fields are not invented.
Columns are appended without changing schema, retention, launcher environment,
or existing field meanings, so older replay readers can ignore the extension.

Presenter-reported submission time is used only when valid inside the observed
native-operation bracket; otherwise the worker boundary is used. Present return
time is not silently promoted into scanout time. Windows present-ready timing and
Linux output polls are CPU completion-observation brackets rather than
hardware timestamps. Source retirement alone remains outside those shared fields. The trace therefore cannot recover an
exact Linux hardware GPU-completion time or an exact Windows completion instant
inside the cadence hold.

### 13.2 What exact replay means

The parser supports schemas 3, 4, and 5, but the inspected strict
`fidelity.baseline_exact` gate requires schema 5. Some operational prose still
calls the launchers schema-4/replay-grade. Inspect actual captured schema and
gate results rather than trusting that label.

The baseline reconstructs the controller using recorded parameters, arrivals,
execution costs, configuration, and lifecycle. Exactness includes complete
sequence/footer accounting, valid timing relationships, required native/GPU
fields, matching controller decisions/diagnostics, simulated submissions,
refresh/raster classifications, and valid execution residuals. A JSON file's
existence or mostly matching timestamps is insufficient; require process exit
success and inspect `capture.recorded_sequence_integrity_valid` and
`fidelity.baseline_exact`.

Exactness proves deterministic reproduction within the recorded model and
evidence. It cannot repair incorrect instrumentation, such as a requested native
parameter recorded differently from the actual API argument. It does not prove
that a candidate policy would cause the same real host, network, GPU, or panel
events.

Metal rows use native backend value 4 and retain local serials, delayed drawable
display events, and non-D3D GPU-ready completion brackets. Replay audits Metal's
acceptance result and rejects DXGI-only evidence on these rows. The worker fixture
crosses requested latch decisions while the native mode remains fixed; exact
replay must retain its controller decisions, submission timing, feedback identity,
and diagnostics. The Metal trace audit additionally rejects tampered backend,
result, ID, event-kind, DXGI, and readiness evidence. These synthetic checks do
not establish native panel refresh or satisfy the DXGI raster gate.

Current-policy replay and queue simulation select the shared prediction policy
regardless of native backend. Exact replay continues to use recorded parameters,
including historical Linux thresholded-event demand. Replay audits Vulkan's
historical texture-poll readiness rows with their recorded result and completion-
bound rules. Asynchronous Linux VAAPI captures can leave output readiness
unavailable; replay must not invent completion evidence for those rows. The separate strict Windows/raster diagnostic gate remains backend-specific
and may still reject otherwise reproducible Vulkan captures when its Windows-only
display evidence is absent.

### 13.3 Counterfactual model limits

Fixed replay retains recorded frame admission and lifecycle while changing
controller decisions. The `worker-occupancy-v1` decision-time model shifts
candidate decisions according to simulated prior submission and captured
post-submission gaps when the recorded worker was occupied. This improves on
reusing a stale decision time, but it is not a complete alternate execution.

A changed policy could change live stale-frame shedding, decoder backpressure,
queue admission, acquisition behavior, GPU cost, and later occupancy. Fixed
replay cannot synthesize all those changes. Worker-mode auditing checks candidate
capacity but does not provide a complete alternate renderer lifecycle or the
same raster simulation readiness.

For admission and discard changes, use the actual worker's deterministic backlog
tests. `vrrqueuesim`'s all-arrival event simulation is exploratory: it shares the
later stale checks but omits early queue pruning and service-aware decode-age
handling, and reuses captured service samples in sequence;
it does not reproduce counterfactual native blocking, decoding backpressure or
physical scanout. Its raw presented jerk, source-spacing residual, drop clusters
and latency distributions are distinct metrics. Unsupported display-only
injections are rejected. Do not treat fixed replay's unchanged admission or
its `stock_*` row as an actual fixed-refresh session.

`worker_saturated` identifies scenarios whose candidate occupancy shift exceeds
the model's useful cadence range (the implementation uses a median shift over
one source period). Their latency/cadence results must not be treated as valid
live predictions because fixed admission cannot shed frames like the worker.

Raster replay models software-visible phase exposure from native anchors,
geometry, and probe brackets. It can compare modeled VRR-following and
free-running scenarios. `optical_tear_confirmation_available` remains false.

## 14. Metrics and a useful investigation method

The overlay's `Incoming smoothness (host)` uses the last 30 valid source frame
intervals. Compute their population variance around their own mean, rather
than an expected interval derived from the requested FPS. For standard deviation
`sigma` in milliseconds, the score is `100 / (1 + (sigma / 6)^4)`. This soft curve
assigns almost no penalty to small variation: 1, 2, and 3 ms standard deviations
score 99.92%, 98.78%, and 94.12%. The 6 ms knee is a UI heuristic, not a measured
perceptual threshold or a probability of noticing stutter. It is deliberately
independent of the controller's 3 ms native hitch threshold.

Stable 30, 50, 60, or 120 FPS all score 100%. A 60-to-50 FPS step remains above
99.4% even while the window contains both rates, and settles to 100% after 30
new intervals. Larger cadence changes can temporarily lower the score while
both rates are in the window; timestamp data alone cannot establish intent.
Source stalls are included and age out after 30 subsequent intervals.

Measure raw host RTP intervals at decode-unit ingress before decoding and
pacing, with no local arrival timestamps or fallback presentation timestamps.
Missing, duplicate, or out-of-order frames and repeated/backwards timestamps
clear the window; normal RTP and frame-number wrap remain valid. Require 30
complete intervals (31 consecutive frames) before displaying a percentage;
otherwise display `N/A`. The tracker owns the window across overlay refreshes.
Stats aggregation selects the newest sequence-tagged snapshot, including a
newer unavailable result, instead of widening the window or averaging scores.
The existing roughly one-second overlay refresh cadence is unchanged; the
session-end log likewise shows the final window, not a whole-session percentage.
This identifies uneven host-supplied timing, which includes capture behavior;
it cannot isolate the game engine or detect repeated image content from timing
alone. It is independent of the native-confirmed client hitch metric and does
not change buffer adaptation.

For historical revision 3-5 policies, the stats overlay and session summary show
client readiness over a rolling 30-second outcome window, alongside the selected
target and a buffer-limit indicator. Revision 3 and later measure preparation completion against the intended
smooth deadline before late-readiness and display-floor recovery clamps.
Client playback drops count as misses; deliberate shutdown, suspension, and
interruption discards are excluded. A second row reports the percentages late
by more than 1 ms and 2 ms and the 30-second drop count. Drops have no invented
lateness magnitude. Window snapshots are selected by timestamp, never summed
across overlay refreshes. The window uses 100 ms buckets and starts with the
available outcomes before 30 seconds have elapsed. This remains a readiness
measurement, not a visible-smoothness score. With no eligible frames, the line
shows the starting state. Revision 4 applies the same thresholded-miss policy to
this score: through 1 ms is on time, 1-2 ms counts only above 50% prevalence,
and over 2 ms or a drop always counts. Production revision 7 instead reports
the interval buffer's one-second mean error and severity-weighted quality over
the preset's history window; these are not that older readiness percentage.
Tolerance is applied to the one-second mean before severity weighting. Both
the report and buffer control use this averaged score and the displayed target.
Isolated spikes can disappear beneath tolerance; this is a buffer-control quality
measure, not the share of perfect frames or a guarantee of visible smoothness.
Unobserved sequence gaps remain excluded and drops are reported separately.
Historical revisions 9/10 retain per-interval reporting for exact replay.

With deep tracing off, the overview retains the VRR17 frame queue delay,
rendering time, incoming host smoothness, VRR pacing/smoothness target, and
interval-error rows. The historical Smoothness label still denotes the client
interval-quality score, not measured physical display smoothness.

Live statistics timing graph (2026-10-03, over `92119295` plus this worktree):
the frametime graph is a separate overlay toggle from the stats text
(`showFrametimeGraph`, default off; Ctrl+Alt+Shift+F or Select+L1+R1+Y while
streaming). `OverlayManager` keeps separate stats and graph flags; the debug
overlay is enabled while either shows, so renderers need no new overlay type, and
the graph can show without the text. It draws three aligned lanes, each with
the raw interval of the newest 240 frames: Planned cadence (gray, target to
target), Client submissions (cyan) and Display events (magenta, OS-reported
`DisplayEvent` times). All lanes share one millisecond axis centred on the
median planned interval with a +/-2 ms range, so a disturbance appears only in
the lane where it enters the pipeline. Intervals within 1 ms of the reference
(`TimingGraphLayout::FlatUs`) are drawn on the reference line, because that
variation is not noticeable; larger ones are drawn at their true value, and
captions keep raw values. A spike in Display events with flat
Planned/Client lanes was added after submission (GPU, compositor or display
scheduling). Values beyond the axis are clipped with red edge markers, and each
caption gives the latest interval and window peak; the display caption also
counts matched intervals. `timingGraphInterval()` defines each lane. A display
interval needs feedback for both frames of the same epoch and backend, so a
missing presentation is a gap, never one long interval. Planned intervals
include buffer steps, which therefore appear as matching planned and submission
spikes.

Present timing issues (2026-10-04): `Vrr13::PresentTiming` counts, over a rolling
30 s window ending at the newest scored interval, display intervals that the
display path made uneven. The plan is each frame's original scanout time
(`decision.originalScanoutUs`, which `PresentationPrediction` already keeps per
pending submission and now passes to its callback with the submission time).
For adjacent frames, the display error is |displayed interval − max(planned
interval, display period)| after subtracting both samples' uncertainty, and the
submit error is |submitted interval − planned interval|. An interval counts when
the display error exceeds the interval tolerance and exceeds the submit error by
more than the tolerance. Displays that absorb uneven submissions by queueing are
therefore not blamed, and neither is spacing held at the panel's fastest
refresh. A hitch is a counted interval whose added error is at least one planned
frame (and at least one display period). The window also keeps the worst added
error. A few large present stalls barely move the percentage: on the
2026-10-04 10:35 4K PyroWave capture, five on-time submissions reached the
screen about 25 ms late, which is 0.03% of intervals. The overlay therefore
reports `Hitches` and `Worst` beside it. When the source period or the submitted interval exceeds
`vrrFloorLatchGapUs` (20 ms, the controller's existing LFC assumption), the
panel may be repeating frames on the driver's low-framerate-compensation
schedule. Scoring then pauses for that interval and for 250 ms afterwards,
because the driver leaves that mode based on its own average frame time.

An earlier latency-baseline definition (display − max(submit + minimum recent
latency, previous display + period)) reported 98% on a 4K PyroWave stream. That
pipeline's submit-to-display latency spans 7–23 ms because frames queue behind
GPU work, so the minimum baseline made almost every frame look late. Measured
with the current definition:
- 2026-10-03 22:56 mixed-rate Deck capture: 0.2% at 91+ fps, 2.0% at 61–90,
  paused below about 50 fps.
- 2026-10-04 1440p PyroWave: 3.3%.
- 2026-10-04 overloaded 4K PyroWave (31% client drops, 10 ms GPU preparation):
  36%.
- Matched clip replays: GPU `auto` 38–51%, sysfs `high` 5.1%, stable-pstate PEAK
  7.6%.

The result rides in `IntervalBuffer::Stats::present` for the overlay. It shows a
percentage when there are fresh scored intervals, `paused below VRR range` when
only paused intervals are fresh, and otherwise unavailable. It is diagnostic only:
production revision 7 scores Smoothness from submission intervals and keeps
native-hitch adaptation off. Display misses therefore neither lower Smoothness
nor grow, hold or release the buffer. Targets, buffer policy, trace schema and
exact replay are unchanged.

This is available during ordinary VRR streams without tracing.
`TimingGraphHistory` keeps 256 observations, allocated once when the pacer is
created. Normal worker outcomes populate it under the existing telemetry lock;
there are no new graphics-driver calls or per-frame allocations. While stats are
enabled the decoder copies the newest 241 points (one predecessor for the first
display interval) at 10 Hz and with each roughly one-second text refresh. The
overlay worker draws the lanes into the existing debug-overlay surface shared by
the surface-based renderer APIs, so renderer overlay uploads occur at up to
10 Hz while stats are shown. Controller targets, buffer policy and trace schema
are unchanged by this chart.

Windows D3D11 overlay publication (2026-10-04): the renderer retains the last
complete texture, shader-resource view and vertex buffer while the overlay
worker creates a replacement. It swaps all three references together only
after successful creation, and releases retired resources outside the lock.
Resource-creation failures preserve the previous graph. Rendering takes the
short reference lock instead of skipping the overlay on lock contention;
window-resize resource reconstruction is already excluded from rendering by
the presentation lock. The previous remove-before-upload path left graph-free
frames during each refresh, causing visible flicker at the graph's 10 Hz
update rate. The incremental Windows release build, overlay suite and six required
VRR suites pass. The ChaseShare portable tree and ZIP were updated with matching
SHA-256 hashes and a successful UNC replay help check. Live visual confirmation
of the graph during streaming remains pending.

At 4K drawable output (at least 3840 by 2160 pixels, in either orientation), the
stats font increases from 20 to 26 pixels. Graph dimensions, strokes, text wrap
width and outline size scale with it by 1.3, rather than stretching a pre-rendered
bitmap. Initial renderer attachment and window size/display changes update this
policy; lower-resolution outputs return to normal size. Modern SDL queries
physical drawable pixels, including high-DPI windows; older SDLs use GL drawable
size or ordinary window size. Font replacement is owned by the overlay worker,
and revision validation discards stale work during output changes. Status-message
font sizes remain independent.

Display events are associated by backend, submission ID and presentation epoch;
delayed feedback updates the earlier frame's observation. Only `DisplayEvent`
timestamps are accepted. DXGI `RefreshReference` samples, Present return times
and modeled scanout are excluded. Missing or nonmonotonic display samples leave
gaps, and a backend without feedback shows `Display events: unavailable`. These remain OS
presentation observations, not physical-panel timing. Source rebases/phase
discontinuities break the curves and ID matching, while ordinary client drops
remain visible in the next measured submission interval. A fresh live stream is
required to verify readability and GPU/upload cost on a particular device.

GPU performance hold (2026-10-03, over `92119295`): with Settings > **High-performance
GPU power while streaming** (`highPerformanceGpuPower`, default off), `Session::exec()`
holds a `GpuPerformanceHold` for the stream. On Linux it selects amdgpu render
nodes whose device has a connected connector and whose
`power_dpm_force_performance_level` is `auto`, allocates an amdgpu context on
the render node and requests `AMDGPU_CTX_STABLE_PSTATE_PEAK`, then reads the
state back. The request is unprivileged, works from the rootless development
container, and is owned by that context: freeing it (at stream end or process
death) restores the level captured at context creation. A sysfs write during the
stream (Steam's manual GPU clock, for example) clears context ownership and is
not overridden. Any other existing level is left alone and logged; EBUSY means
another process holds a stable pstate. On the Steam Deck, PEAK fixes GFX at 1300
MHz and FCLK/MCLK at 800 MHz (sysfs `high` reaches 1600 MHz GFX but needs root).
Other platforms and vendors have no equivalent unprivileged per-process request,
so the setting is Linux-only and the hold is a no-op elsewhere. Logs use the
`GPU performance hold:` prefix. This addresses demand-based clock ramping that
delays presentation after on-time submission.

Video thread scheduling (2026-10-03, over `92119295`): the decoder, dedicated
renderer, V-sync, VRR pacer and Vulkan preparation threads request scheduling
priority once through `VideoThreadPriority`. Windows registers each with the
MMCSS `Playback` task (HIGH relative priority for pacing/V-sync, NORMAL for
decode/render/preparation) and falls back to SDL; other platforms use SDL
directly, TIME_CRITICAL then HIGH for pacing/V-sync. Results are logged as
`Video thread priority:`. The final VRR deadline region polls with SDL's CPU
pause hint instead of an OS scheduler yield; the coarse sleep, the 500 us base
region plus at most 500 us learned wake lead, targets and buffer policy are
unchanged, and trace `active_yield_count` fields now count polling steps. No GPU
scheduling priority is requested: it orders work between GPU contexts, does not
change clocks, and amdgpu denies above-normal priority without `CAP_SYS_NICE`.

Advanced stats follow the worker's `MOONLIGHT_VRR_DEEP_TRACE` switch (value
starting with `1`), including the Settings tracing checkbox and external deep
trace launchers. Merely enabling an ordinary trace path does not expand stats.
Read the current Qt process environment, not SDL2-compat's cached environment,
so Settings tracing enabled before connection takes effect in both places.

The advanced VRR overview leads with applied buffer and its limit, followed by a plain
language explanation of the interval observer's latest action. It distinguishes
late-frame growth, capped growth, current timing pressure, unabsorbable work, a
remaining recent-pressure clean-time hold, release, minimum, and qualification.
The reason describes the request for subsequent frames, not a diagnosis of a
particular GPU/network fault. The client timing score and one-second error follow.

The average delay block separates GPU decode synchronization, frame queue time
(queue residence plus pacing/other), and rendering (preparation plus submission
call). These retain the existing accounting and successful-frame denominator.
An explicit GPU-ready wait is reported with measurement coverage and averages
only frames with a valid sample. On Windows the residual present-ready wait is
inside the submission call and therefore also inside rendering; the GPU-queued
decode dependency is not a separate CPU wait. Linux VAAPI Mailbox output uses GPU
presentation synchronization and has no CPU output-ready sample. Other Linux
imports and software frames report their completion poll inside preparation.
These rows do not measure total GPU execution. Applied buffer is a
schedule allowance, not another component to add to these measured times. The
normal and non-VRR overviews retain their original queue/rendering rows. Request values,
growth/capped-step ages, calibration counts, GPU head-start budgets, protected
submission share and submission jerk remain available in diagnostic telemetry
instead of crowding the overview. Neither timing quality nor submission jerk is
physical display smoothness. The trace extension
records the attributed late frame, attempted and clipped growth, and hold/
cooldown time. It does not change requested or applied delay. In particular,
the historical capacity flag can miss revision-7 growth that was already
clipped inside the observer; `buffer_clipped_increase_us` exposes that loss.

Submission cadence counts eligible
consecutive submission-predicted intervals with client-added spacing error over
3 ms. Motion jerk counts adjacent submission intervals differing by over 2 ms,
including host cadence changes, source stalls, and local drops. Neither proves
physical scanout or drives buffering. Native display/compositor intervals remain
separate tracking evidence. Cumulative counters are differenced into decoder
reporting windows, independently of the controller's five-minute histogram.
Failed-presentation diagnostics also remain internal.

Visible smoothness and source-timestamp fidelity answer different questions:

```text
presentedInterval[i] = presentedTime[i] - presentedTime[i-1]
presentedJerk[i]     = change between adjacent presented intervals
senderResidual[i]    = presentedInterval[i] - corresponding RTP interval
```

Use the replay's in-process `replay_presented_jerk_*`,
`original_presented_jerk_*`, and `stock_presented_jerk_*` fields, including tail
values and the share above 2 ms, when discussing overall visible cadence.
For the controller-only 99.95% goal, game-driven interval changes are not
failures. The new `simulation.sender_cadence.client_spacing_accuracy_percent`
uses a strict error greater than 3 ms; `client_spacing_errors_over_3ms` and
`client_spacing_pairs` expose the exact numerator and denominator. It excludes
source intervals over 25 ms, counts them separately as `source_stall_pairs`,
and does not exclude long local arrival gaps when RTP is steady. The older
`spacing_accuracy_percent` and 2 ms fields retain their historical contract,
including their sender/arrival exclusions, for comparison.

These replay spacing fields use submission timing as a presentation proxy.
Current revision 7 uses averaged submission-interval error with readiness attribution to
control padding; native confirmation remains diagnostic. Historical prediction-
only policies instead derive padding from readiness prediction.
Report `smoothness_feedback.native_window_samples` and `native_window_misses`
separately. Sparse or missing native observations cannot establish 99.95%
visible smoothness, even when the observed miss count is zero. Counterfactual
native timing retains recorded service latency shifted with candidate submissions.
Raw presented jerk also includes the game's cadence and must be reported without
attributing all such motion to the client.

Desktop/idle captures are not gameplay tuning evidence. Confirm that the latest
capture contains the workload being optimized before selecting a latency versus
smoothness tradeoff. Keep source stalls, pre-arrival delivery gaps, decoder work,
and post-submission blocking separate when interpreting a sweep. A growing buffer
cannot necessarily fix a delay that moves with the submission target.

For an actual capture investigation:

1. Re-enumerate both `%USERPROFILE%\vrr-traces` and
   `\\allytwo\ChaseShare\vrr-traces` immediately before analysis. Use the newest
   completed capture unless the user names one. Record full path, length,
   UTC modification time, and SHA-256. Match sidecars by complete basename.
2. Run a fresh exact baseline with the current replay executable and check the
   actual exit code and fidelity fields. Failed exactness means exploratory
   evidence, not strict A/B proof.
3. Keep untouched baseline and candidate output separate. Batch named scenarios
   in one versioned config; use native replay parallelism instead of an outer
   shell loop. Automatic jobs are capped at 16.
4. Compare jerk/cadence tails, decode-to-submission mean and p50/p95/p99,
   submission drift, modeled spacing violations, raster bounds, and saturation.
   Report excluded source stalls and evidence gaps.
5. Choose a useful latency/smoothness tradeoff, then exercise nominal and injected
   decision/preparation/submission/scheduler disturbances with explicit interval
   and latency bounds. Severe fault latency is not normal operating latency.
6. After the final code change, rebuild the relevant diagnostic binaries and
   rerun candidate/stress results on the exact same trace. Pre-rebuild output
   cannot validate the final source. Generate a timeline only when per-frame
   causality or unavailable summary statistics require it.

Distinguish observed symptoms by boundary: packet/frame loss, assembly delay,
decode service, GPU dependency wait, preparation/acquisition, scheduler lateness,
intentional queue protection, submission behavior, and native/display evidence.
An average FPS counter alone can conceal all of these.

## 15. Tests, deployment boundaries, and maintenance

For opt-in Linux bandwidth testing, [moonlight-link-test.py](scripts/moonlight-link-test.py)
redirects incoming IPv4/IPv6 traffic on a selected interface through an IFB with a bounded TBF
queue (default 1000 Mbps, 2 ms of queue service). It exercises packet delivery
and reassembly before decode; existing frame-level VRR replay cannot model this
bottleneck. It is an approximate software link, not physical gigabit timing,
and does not alter production policy. An optional host filter limits only that
host's incoming UDP. See [network-link-testing.md](docs/network-link-testing.md)
for activation, counters, limitations and removal. Isolated Linux namespace
checks cover IPv4/IPv6 setup/removal, real IPv4 UDP redirection and overflow,
host isolation, existing-rule refusal and failed-setup rollback; live gameplay
and sustained gigabit throughput remain separate validation.

The deterministic suites are
[tst_vrrtimingcontroller.cpp](tests/vrr/tst_vrrtimingcontroller.cpp),
[tst_vrrratepolicy.cpp](tests/vrr/tst_vrrratepolicy.cpp),
[tst_vrrpacingworker.cpp](tests/vrr/tst_vrrpacingworker.cpp),
[tst_vrrreplayconfig.cpp](tests/vrr/tst_vrrreplayconfig.cpp),
[tst_vrrrenderpolicy.cpp](tests/vrr/tst_vrrrenderpolicy.cpp), and
[tst_d3d11bindpolicy.cpp](tests/vrr/tst_d3d11bindpolicy.cpp).
The last covers the 4K decoder-bind vs compatibility-copy rule.
They cover timing arithmetic, timestamp wrap/rebase, cadence changes, delay and
history behavior, queue/drop/cancellation/suspension, ownership, wait floors,
trace integrity, native diagnostic modeling, and replay configuration/contracts.
Consult the test names and assertions for the specific behavior being changed;
the existence of a broad suite is not proof that a native API argument is tested.
`tst_vrrdiagnostics` additionally exercises capture scope, environment restoration,
export locking, file preservation and ZIP bounds; the independent Python ZIP
check verifies contents and CRCs. Windows CI includes that test and exact replay
of cold and warm-history worker fixtures under the unchanged production policy.

The ordinary application build does not build the opt-in replay/tests.
Follow AGENTS.md to build diagnostics separately and run all four suites plus
`vrrreplay --help` when required. Missing runtime DLLs are environment failures,
not pacing failures. The macOS native presenter smoke test exercises the actual
Metal/VideoToolbox renderer separately from the hardware-free worker fixture.
The display-range test checks range validation independently of a particular
attached display. See [Mac build and use](docs/macos-vrr-pyrowave.md) for build
commands and validation outcomes. No deterministic test here establishes optical
tearing, full host behavior, or physical A/V synchronization.

Mac validation (2026-10-04, Apple M5 Pro): 17 deterministic VRR suites and
seven CPU PyroWave/haptics suites pass. Native shared-plane regression passes
12 cases through 4K and 3024x1964 with exact Metal/Vulkan byte agreement,
exhaustion, poison/reuse and retained-lifetime checks. The native calibration
smoke passes 288 draws across eight format/resolution combinations through
1080p into a 1440p target, including grayscale/color/alpha and matrix checks.
An actual encode/framing/asynchronous shared-decode-to-Metal smoke presents
128x96 4:2:0 8-bit and 4:4:4 10-bit patterns adaptively, including cancellation,
source recycling and decoder teardown. A 4:4:4 10-bit shared frame also passes
the fixed/display-link path.
Its native worker fixture submits 100 synthetic frames at 116 FPS with no worker
drops and 97 matched native display intervals. The clean-close schema-5 trace
passes exact replay, as do success/failed-preparation Metal feedback fixtures;
11 corrupted-evidence checks remain rejected. These validate native ownership,
rendering and diagnostic contracts without a live host, network or game.
External-display behavior, sustained live throughput and physical display/A/V
results remain unmeasured. Runtime evidence here is arm64; the PyroWave static
library also cross-compiles for x86_64, without an Intel runtime test.

For the current Windows setup, ALLYTWO is this client and also the SMB server;
the Sunshine host is another LAN machine. The private live portable installation
is `\\allytwo\ChaseShare\MoonlightPortable-x64-6.1.0-vrr-lite`.
Keep its stable name. `AllyShare` is an open host-log drop and must not receive
release builds or profile/settings data.

An incremental local link is not publication. A requested ChaseShare update
also needs staging, changed diagnostics, refreshed ZIP, process-safety checks,
complete copy, source/live hashes, and the UNC replay smoke test. Preserve
`portable.dat.inactive`, dependencies, and tools; do not overwrite a running
gaming installation. Follow the complete commands in AGENTS.md rather than
reconstructing them from this architectural summary.

When maintaining this document:

- Update the baseline and affected explanations when active behavior changes.
- Follow resolver values through effective formulas, not only declarations.
- Follow intended native parameters through the actual API call and telemetry.
- Keep live policy, fallback behavior, historical replay modes, and optional
  experiments distinct.
- Preserve clock units, frame identity, queue ownership, and lifecycle order in
  every new diagnostic or model.
- Revisit exact replay whenever schema, feedback, policy state, or execution
  boundaries change; update both share launchers when their contract changes.
- Keep native Present arguments and telemetry aligned; validate the native call
  boundary and real Windows behavior before interpreting optical results.

The durable debugging approach is to trace an observed frame through the full
chain, identify the first boundary that differs from its intended behavior,
and check how that difference propagates into subsequent frames and feedback.
That keeps host stalls, local overload, scheduling policy, native behavior,
and measurement limitations from being conflated.


### Historical buffer-lag correction, 2026-09-10 (Linux growth policy retired 2026-09-11)

This update supersedes the expanding maximum and prediction-only Linux growth
policy described above. The absolute playout maximum is now 16 ms; a lower
source rate cannot expand it. Source-relative preset limits can still reduce it.

Linux enables `readinessHitchFeedback`, recorded as
`playout_readiness_hitch_threshold_us=2000`. Consecutive eligible submissions
must have a spacing error attributable to decode/preparation readiness beyond
the intended target before buffering can grow. Demand uses the delayed frame's
existing buffer plus the smaller of readiness lateness and spacing error, minus
2 ms. Catch-up uses the earlier frame's buffer, avoiding duplicate charges.
Host jitter, native blocking, or cached predictions alone cannot authorize growth.
Work or decoder backpressure beyond a source period breaks eligibility.

Version-19 reserve history retains/releases demand but cannot raise a cold-start
request by itself. It rejects old prediction profiles and does not add a second
3 ms margin. Existing attack/release and capacity bounds remain. Windows retains
its predictive growth rule. The new parameter defaults to zero for historical
traces; replay recognizes version-19 profiles and selects the current Linux rule
for declared Vulkan captures. Submission attribution is not physical scanout proof.

Controller, profile round-trip, replay configuration, and low-rate cap regressions
cover this policy. Live Desktop Mode evidence showed buffering releasing rather
than remaining at the cap; Windows-level visual parity remains unverified.

### Shared buffer policy and queue-stat accounting (2026-09-11)

The historical 2026-09-10 Linux readiness-hitch sections above describe the retired policy.
Production session setup leaves `readinessHitchFeedback=false` on both platforms,
selecting responsive adaptation with version-20 diagnostic history. The earlier
shared version-18 prediction-plus-margin law remains replayable.
Current-policy replay also selects this shared policy regardless of captured
backend; exact replay and explicit scenario parameters preserve version 19.
The changed queue statistic subtracts the explicit worker decode wait after
bounding it to non-render client time, preventing unsigned underflow. Native
render preparation costs keep their existing classification. This is an accounting
correction, not evidence that the GPU became faster.

Responsive-policy validation (2026-09-11): all four required deterministic
suites and profile tests pass. Eighteen 56-second controller fixtures cover
all presets, smoothing on/off, delivery/render/scheduler faults, and repeated
120/19/30 FPS transitions: clean padding stays at 1 ms and recovers near 1 ms
within eight seconds of the injected faults ending. A synthetic worker capture
passes exact replay and the five-scenario responsive-buffer stress config.
The latest completed live capture (20260911-181549-892) fails original-deadline
replay at frame 8355 in both the old and new binary, exit 3; no live A/B or
physical smoothness improvement is established. Existing history_* trace fields
refer to five-minute diagnostics, not the new live release gate.

### Linux Bluetooth DualSense feedback (2026-09-19)

The Deck client uses the same versioned 0x5601 waveform payload as Vibeshine:
48 kHz stereo S16LE, a sequence number, and at most 240 frames per packet.
The optional receive callback only queues bounded chunks; a separate worker
resamples to signed 8-bit 3 kHz stereo and emits SAxense Bluetooth reports.
Only a controller with an opened, kernel-verified Bluetooth hidraw path
advertises LI_CCAP_HAPTICS_PCM. See third-party/saxense/PROVENANCE.md.

In merged controller mode, startup opens attached DualSense controllers first
so the host's first player-0 announcement describes the actual feedback target.
Multi-controller numbering retains enumeration order. Rumble and adaptive
trigger feedback are routed by player index to the matching DualSense; SDL
remains responsible for input and non-waveform effects. Waveform teardown joins
its worker before closing SDL's controller handle. Physical feedback still
requires live validation; packet writes and tests do not establish sensation.

Deck validation: the native Qt build and waveform worker tests passed using
sdl2-compat over SDL 3.4.12. A live Desktop stream announced the Bluetooth
DualSense first (player 0, PlayStation, capabilities 0x80fb); host tracing
confirmed negotiated feature flags 0x7. One second of silent four-channel
48 kHz audio written to the virtual DS5 ALSA endpoint reached the client's
PCM callback and produced a 142-byte Bluetooth report. This proves the silent
transport path only. Physical rumble, adaptive-trigger resistance and native
007 First Light waveform output await user confirmation. The previous
binary is /tmp/moonlight-pre-codex-ds5 and the pre-edit source snapshot is
/tmp/moonlight-before-codex-ds5.tar.gz, with the common-c diff separately kept
in /tmp on the host workstation. No existing game/Steam settings were changed
by this Deck repair.

A launch request also carries a bitmap identifying attached PlayStation
controllers. Vibeshine can use it to delay a direct Proton title until its
virtual DualSense and Sony audio endpoint have enumerated, avoiding a one-time
game startup race without delaying launches for other controllers.

### Recovery validation scope (2026-09-20)

The worker now rejects expired queue fronts before decode synchronization. This
avoids spending GPU-wait time on images that were already too old to present.
Exact replay preserves these `queue_stale` terminal rows without advancing the
controller. `vrrqueuesim` does not yet model this early pruning; its counterfactual
backlog/throughput results cannot validate this recovery. Validation requires the
blocking-decode regression, exact worker fixture replay, and a fresh live stream.
No further buffer-number optimization is part of this recovery.

The subsequent completed `20260920-000347-249118` live capture exposed a separate
starvation loop: submissions stopped for 4.630549 seconds between source frames
486 and 1042 while the worker repeatedly synchronized decode and discarded the
now-ready frame on elapsed age. Decode waits themselves were tens of milliseconds,
not a multi-second blocked call. Immutable decoder-output mapping had also been
applied to discard age, making local GPU service count as replaceable backlog.
Current discard checks subtract only the current frame's explicit decode wait;
clock mapping, `stale_age_us`, full latency reporting, preset caps, and genuine
queue-age rejection stay intact. The deterministic reproduction fails before this
fix and passes afterward in all three presets. A worker trace exported with
`MOONLIGHT_VRR_TEST_EXPORT_CONTENTION_TRACE` covers exact replay separately from
the pre-decode pruning fixture. This establishes software progress in that
scenario, not 120 FPS throughput or smooth physical scanout under live capture.
The final native build and offscreen help pass. All nine VRR/backend suites pass,
and exact replay passes for the selected live trace plus contention, early-queue-
discard, and Windows cancellation-fence worker fixtures. Windows source changes
remain uncompiled here; fresh native integration tests are still required.
