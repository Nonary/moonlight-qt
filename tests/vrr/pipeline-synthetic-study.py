#!/usr/bin/env python3
"""Known-input pipeline tests against the real queue/controller simulator.

The oracle uses future knowledge and is a lower bound, not an implementable
controller. GPU readiness here is exact by construction; captured observations
are upper bounds. Neither model establishes real GPU scanout smoothness.
"""
import argparse
import concurrent.futures
import csv
import json
import math
import random
import subprocess
from pathlib import Path


def stats(values):
    values = sorted(values)
    return {'count': len(values), 'mean': sum(values) / len(values),
            'p99': values[math.ceil(len(values) * .99) - 1], 'max': values[-1],
            'over_500us': sum(x > 500 for x in values)}


def generate(rate, profile, seed, count, warmup=0):
    rng = random.Random(seed)
    decode_end = 0
    source = 2000000
    rows, ideal, available, costs = [], [], [], []
    for i in range(count):
        fps = ([116, 40, 90, 30, 116][min(4, i * 5 // count)]
               if profile == 'rate_changes' else rate)
        source += 1000000 / fps
        source_us = round(source)
        jitter = 0
        gpu_extra = 0
        render = 1100
        if profile == 'bounded_1ms': jitter = rng.randrange(1001)
        elif profile == 'bounded_4ms': jitter = rng.randrange(4001)
        elif profile == 'alternating_6ms': jitter = (i % 2) * 6000
        elif profile == 'correlated': jitter = (i // 40 % 2) * 4000 + rng.randrange(501)
        elif profile == 'stalls':
            jitter = rng.randrange(1501) + (20000 if i % 503 == 400 else 0)
            gpu_extra = 6000 if i % 337 == 300 else 0
            render += 7000 if i % 701 == 650 else 0
        elif profile == 'rate_changes': jitter = rng.randrange(3001)
        # Ordered serial decoding; network delay and GPU work can overlap
        # between frames. No synthetic packet reordering through the decoder.
        decode_end = max(source_us + 3000 + jitter, decode_end) + 600
        ready = decode_end + 4300 + gpu_extra
        rows.append(dict(arrival_sequence=i+1, frame=i+1,
            rtp_timestamp=round(source_us * 90 / 1000), rtp_valid=1,
            decode_complete_us=decode_end, pacer_arrival_us=decode_end,
            gpu_ready_observed_us=ready, display_refresh_hz=120,
            stream_rate_hz=116 if profile == 'rate_changes' else rate,
            additional_queued_frame=0, prepare_us=render,
            present_call_us=100, controller_call_us=10))
        ideal.append(source_us)
        available.append(ready)
        costs.append(render)
    # Necessary fixed phase for S_i + D >= A_i + C_i on every frame.
    delay = max(a + c - s for a, c, s in zip(available, costs, ideal))
    floor = 8333 + 100  # Stated analytical spacing assumption, not a fit.
    feasible = all(b - a >= floor for a, b in zip(ideal, ideal[1:]))
    immediate = []
    for a, c in zip(available, costs):
        immediate.append(max(a + c, immediate[-1] + floor if immediate else 0))
    intervals = [b - a for a, b in zip(immediate, immediate[1:])]
    oracle = dict(fixed_phase_us=delay, cadence_feasible=feasible,
        source_to_submit_us=delay,
        decode_to_submit_us=stats([s+delay-r['decode_complete_us'] for s,r in zip(ideal,rows)][warmup:]),
        ready_to_submit_us=stats([s+delay-a for s,a in zip(ideal,available)][warmup:]),
        immediate_interval_change_us=stats([abs(b-a) for a,b in zip(intervals,intervals[1:])][warmup:]),
        assumptions='Exact source cadence, readiness and CPU render costs; future maximum; no asynchronous GPU/compositor variation.')
    assert all(s + delay >= a + c for s, a, c in zip(ideal, available, costs))
    return rows, oracle


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--output', type=Path, required=True)
    parser.add_argument('--config', type=Path, required=True)
    parser.add_argument('--frames', type=int, default=4500)
    parser.add_argument('--warmup-arrivals', type=int, default=0)
    args = parser.parse_args()
    if not 0 <= args.warmup_arrivals < args.frames - 2:
        parser.error('warmup must leave at least three scored frames')
    args.output.mkdir(parents=True, exist_ok=True)
    profiles = ['clean', 'bounded_1ms', 'bounded_4ms', 'alternating_6ms', 'correlated', 'stalls']
    cases = [(rate, profile, seed) for rate in [30, 40, 60, 90, 116]
             for profile in profiles for seed in ([7, 19, 43] if profile in ['bounded_4ms', 'stalls'] else [7])]
    cases += [(116, 'rate_changes', 7), (140, 'clean', 7)]

    def run(case):
        rate, profile, seed = case
        name = f'{rate}fps-{profile}-seed{seed}'
        rows, oracle = generate(rate, profile, seed, args.frames, args.warmup_arrivals)
        trace = args.output / f'{name}.csv'
        result = args.output / f'{name}.json'
        with trace.open('w') as stream:
            writer = csv.DictWriter(stream, fieldnames=rows[0].keys())
            writer.writeheader()
            writer.writerows(rows)
        subprocess.run(['build-tests/vrr/vrrqueuesim', str(trace), '--config',
                        str(args.config), '--output', str(result),
                        '--warmup-arrivals', str(args.warmup_arrivals)], check=True,
                       stdout=subprocess.DEVNULL)
        replay = json.loads(result.read_text())
        for scenario in replay['scenarios']:
            assert scenario['presented'] + scenario['total_drops'] == len(rows)
            assert scenario['decode_to_submission_us']['p50'] >= 4300 + 1100
        summary = dict(name=name, fps=rate, profile=profile, seed=seed,
                       oracle=oracle, scenarios=replay['scenarios'])
        print(name, 'oracle_phase_us', oracle['fixed_phase_us'],
              'feasible', oracle['cadence_feasible'], flush=True)
        return summary
    with concurrent.futures.ThreadPoolExecutor(max_workers=3) as executor:
        results = list(executor.map(run, cases))
    (args.output / 'scores.json').write_text(json.dumps(results, indent=2)+'\n')
    # Identical alternating jitter and fixed work need identical phase at
    # every feasible FPS. Spare spacing does not erase the jitter amplitude.
    alternating = [r['oracle']['fixed_phase_us'] for r in results
                   if r['profile'] == 'alternating_6ms']
    assert len(set(alternating)) == 1
    assert not results[-1]['oracle']['cadence_feasible']


if __name__ == '__main__':
    main()
