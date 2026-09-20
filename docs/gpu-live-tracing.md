# Live VAAPI/Vulkan diagnostics

These diagnostics observe the current implementation; they do not modify replay,
buffer policy, deadlines, synchronization decisions, or rendering quality.
Enable the existing **Trace VRR frames for debugging** setting, or launch the
native `moonlight-dev` shortcut, which already enables deep tracing. Close and
reopen the client after rebuilding. Each Vulkan renderer instance writes a unique
`<MOONLIGHT_VRR_TRACE>.gpu-<pid>-<UTC milliseconds>-<instance>.csv`. Reconnects and
decoder probes can create multiple sidecars; use those containing actual frame
events. The log prints each path. UI diagnostic ZIP export includes these files.
The existing replay trace and its schema are unchanged.

Revision 2 (`GPU diagnostics v2` in the log) removes the decoder-thread
`decode_output_status` query. In the revision-1 high-bitrate capture, 296 of 297
queries longer than 1 ms returned within 200 us of another frame's decode-sync
completion, consistent with driver serialization. The new header explicitly
records `decoder_output_query=disabled`. The worker-side readiness query remains,
as do synchronization spans and shader timings. There is no diagnostic driver
call on the decoder thread. This removes the observed intrusive probe; a new
live comparison is still required to assess the remaining overhead.

Rows use a bounded, preallocated multiple-producer queue. Formatting and local
file I/O run on a separate writer thread. Producers never wait for the writer.
A footer reports dropped rows and truncation; a missing footer means incomplete
closure. The sidecar stops at 256 MiB and logs a warning. No completeness claim
is valid for missing/truncated/dropped data. Query calls and timer instrumentation
still add overhead, which must be considered when interpreting the run.

## Columns and interpretation

Revision 3 adds passive decoder-thread timestamps around existing FFmpeg calls
and packet handoff, without adding driver calls. The log identifies `GPU diagnostics
v3`. `packet_send`, `decoder_receive`, `decode_sync`, `decode_query_cpu`, and
`render_commands` have `a=result`, `b=thread CPU microseconds`, `c=voluntary context
switches`, `d=involuntary context switches`, and `e=Linux thread ID`. Unavailable
counters are -1. CPU/context-switch observations slightly bracket the wall-time
span and include measurement overhead; CPU time is not GPU execution time, and
wall minus CPU is not a direct GPU queue-delay measurement. A sleeping or
descheduled thread and a driver lock can all produce a long wall/short CPU span.

Additional events:

| Event | Identity and fields |
| --- | --- |
| decoder_input_wait | Wait for the next network-delivered frame; a=success, b=frame number |
| packet_delivery | begin=first packet receive, end=reassembly; a=frame number, b=encoded bytes, c=submitted packet bytes, d=frame type, e=outstanding decoder frames |
| packet_build | Decoder submission entry through packet assembly/statistics; a=frame number |
| packet_send_enter | Marker before entering FFmpeg submission; a=frame number |
| packet_send | Existing avcodec_send_packet call, including failures; a=FFmpeg result |
| decoder_receive | Existing avcodec_receive_frame call, including EAGAIN/errors; a=FFmpeg result |
| decoder_surface | Passive AVFrame metadata; object_id=VA surface ID, a=frame number, b=pixel format, c=width, d=height, e=outstanding frames before dequeue |
| decoder_handoff | Pacer queue admission call; a=frame number |
| decode_sync_enter | Marker before existing vaSyncSurface; object_id=VA surface ID |
| decode_query_cpu | Thread counters for the retained worker-side status query |

Packet events have no decoder-output timestamp yet; join by RTP and source
frame number to `decoder_surface`, then by decoder-output timestamp to worker
events. Only successful receives identify an actual output surface. Failed
receive rows name the next expected output, not a produced frame. Surface IDs
are reusable: always join using frame identity as well. Subtract reassembly
from `packet_build.begin_us` to measure delay reaching the decoder; send return
to decoder output and output to sync entry identify the remaining handoff gaps.
Overlapping packet-send and sync spans on distinct thread IDs can expose
cross-thread serialization; coincident end times are a clue, not proof of a
particular driver mutex. Encoded bytes permit within-run size/wait correlation.

`rtp_pts` is the source's raw 90 kHz RTP timestamp. Join to the main trace using
`decoder_output_us` (and RTP); this disambiguates timestamp wrap and surface reuse.
`begin_us`/`end_us` use `LiGetMicroseconds`, the main trace's CPU monotonic clock.
`detail` gives libplacebo's shader operation names (bounded to 191 bytes) for
shader-history rows, such as color decoding and scaling; it is CSV-quoted.
Rows from different threads can arrive out of timestamp order. Never subtract
GPU nanosecond durations from absolute CPU timestamps.

| Event | object_id | a / b / c / d / e |
| --- | --- | --- |
| decode_wait_status | VA surface ID | query VAStatus / VASurfaceStatus |
| decode_sync | VA surface ID | vaSyncSurface result |
| source_capacity | 0 | success / retained source count |
| acquire | 0 | unused |
| surface_import | 0 | success |
| render_commands | 0 | success |
| render_flush | 0 | preceding render success |
| output_wait_mode | 0 | 1 = asynchronous VAAPI, 0 = existing CPU completion wait |
| shader_history | shader signature | stage / pass index / sample count / last GPU ns / average GPU ns |
| source_retained | 0 | retained source count |
| source_retired | 0 | unused |
| source_teardown | 0 | unused |
| output_status_before_present | presentation ID | 1 = output pending, 0 = complete |
| present | presentation ID | submission success |

Surface status is only valid when query VAStatus is success (0). libva defines
Rendering=1, Displaying=2, Ready=4, Skipped=8. An error is unavailable evidence,
not an unready surface. The output-status query and worker-side VA status query request no completion
wait; their CPU call spans are retained to expose driver overhead or blocking.
Successful `decode_sync` bounds readiness by its return time. It does not measure
the decoder engine's execution duration.

`source_retired` brackets source-read completion between the last busy poll and
the first idle observation. A zero begin means no busy poll was observed; only
the upper bound is known. Teardown observations are separate and must not be
treated as normal frame completion. Source-read retirement is not output-image
completion or scanout. The output query is taken just before normal presentation;
it never waits or changes the existing submission decision.

`shader_history` is libplacebo's GPU timer data for the shader signature. Samples
are returned asynchronously and may repeat between callbacks. The row identifies
the frame invoking the callback, **not the frame producing the GPU sample**.
Zero sample count means unavailable. These durations help distinguish expensive
shader processing from CPU dependency waits, but cannot locate absolute GPU
start times, identify queue delay, or establish exact cross-engine contention.
That requires a driver-level GPU timeline if the new evidence leaves ambiguity.

## User test

Use the same game scene, 1440p/120 FPS, AV1 10-bit, latency preset, display mode,
and overlay state as the reported problem. Record 60–90 seconds at the problematic
high bitrate, disconnect normally, then repeat at the previously good low bitrate
(approximately 57 Mbps). Keep the scene as repeatable as possible. Do not change
other settings between runs. Report the bitrate, perceived stutter and displayed
client timing for each; preserve both main traces, GPU sidecars and session logs.
This is observational testing of the already-built asynchronous path, not proof
of a bitrate effect or a matched comparison to older synchronous captures.
