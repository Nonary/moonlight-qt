#!/usr/bin/env python3
"""A retimed target must not erase measured lateness in the capture audit."""
from pathlib import Path
import sys
sys.path.insert(0, str(Path(__file__).resolve().parents[2] / 'scripts'))
from vrr_deadlines import audit


def frame(i, error, feedback=True):
    base = i * 100000000
    original = base + 10000000
    actual = original + error
    events = [dict(event='arrival', frame_id=i, observed_ns=base, decoded_ns=base),
              dict(event='plan', frame_id=i, observed_ns=base, predicted_scanout_ns=original),
              dict(event='prepared_plan', frame_id=i, observed_ns=actual - 1000000,
                   predicted_scanout_ns=actual),
              dict(event='render', frame_id=i, observed_ns=actual - 2000000,
                   started_ns=base, duration_ns=actual - 2000000 - base),
              dict(event='submit', frame_id=i, observed_ns=actual - 1000000,
                   predicted_scanout_ns=actual, success=True)]
    if feedback:
        events += [dict(event='feedback', frame_id=i, observed_ns=actual + 1000,
                        presentation_ns=actual, uncertainty_ns=1000, quality=2, outcome=0)]
    return events

r = audit(frame(1, 3000000) + frame(2, 3000001) + frame(3, 7000000, False))
assert r['paired_frames'] == 2
assert r['submitted_frames'] == 3
assert r['late_over_3ms_initial_target'] == 1
assert r['late_over_3ms_final_target'] == 0
assert r['targets_shifted_later_over_3ms'] == 1
# An impossible future presentation timestamp is unavailable evidence.
events = frame(1, 5000000)
events[-1]['observed_ns'] = events[-1]['presentation_ns'] - 1
assert audit(events)['paired_frames'] == 0
print('Immutable-target audit, strict 3 ms boundary and missing/invalid feedback checks passed')
