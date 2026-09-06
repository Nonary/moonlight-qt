#!/usr/bin/env python3
"""Replay headroom/render-overlap candidates and score fixed frame identities.

Run from the repository root inside moonlight-dev. CPU timing only: this does
not model asynchronous Vulkan completion or prove displayed smoothness.
"""
import argparse
import concurrent.futures
import csv
import json
import math
import subprocess
from pathlib import Path


def distribution(values):
    values = sorted(values)
    if not values:
        return {"count": 0}
    return {"count": len(values), "mean": sum(values) / len(values),
            **{label: values[math.ceil(q * len(values)) - 1]
               for label, q in [("p50", .5), ("p95", .95), ("p99", .99)]},
            "above_us": {str(t): sum(x > t for x in values)
                         for t in [125, 250, 500, 1000, 2000]}}


def parameters(credit=0, overlap=1000, release=0, shared=1):
    return {"playout_shared_render_deadline": shared,
            "playout_smoothing_gain_per_mille": 200,
            "playout_pipeline_budget": 0,
            "playout_delay_start_us": 6000,
            "playout_delay_maximum_us": 8000,
            "playout_delay_start_period_per_mille": 950,
            "playout_delay_maximum_period_per_mille": 950,
            "playout_delay_percentile_per_mille": 1000,
            "playout_delay_minimum_samples": 250,
            "playout_delay_release_samples": 500,
            "playout_render_overlap_per_mille": overlap,
            "playout_ready_lead_us": 1000 if shared else 0,
            "playout_headroom_credit_per_mille": credit,
            "playout_release_us_per_second": release}


def score(path, metadata, cutoff):
    groups = {}
    previous = None
    prior_interval = None
    with path.open() as source:
        for row in csv.DictReader(source):
            sequence = row['arrival_sequence']
            if sequence not in metadata:
                continue
            elapsed, fps = metadata[sequence]
            submission = int(row['simulated_submission_us'])
            if elapsed < cutoff or not submission:
                previous = prior_interval = None
                continue
            band = '<40' if fps < 40 else '40-59' if fps < 60 else '60-79' if fps < 80 else '80-99' if fps < 100 else '100+'
            interval = submission - previous if previous is not None else None
            for group in ['all', band]:
                g = groups.setdefault(group, {k: [] for k in
                    ['latency', 'buffer', 'error', 'interval_change', 'cadence_residual']})
                g['latency'].append(submission - int(row['decode_complete_us']))
                g['buffer'].append(int(row['simulated_playout_delay_us']))
                g['error'].append(submission - int(row['simulated_target_us']))
                if interval is not None and prior_interval is not None:
                    g['interval_change'].append(abs(interval - prior_interval))
                if row['simulated_cadence_valid'] == '1':
                    g['cadence_residual'].append(abs(int(row['simulated_cadence_residual_us'])))
            previous, prior_interval = submission, interval
    return {name: {metric: distribution(values) for metric, values in g.items()}
            for name, g in groups.items()}


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--output', type=Path, required=True)
    parser.add_argument('--trace', type=Path, required=True)
    parser.add_argument('--csv', type=Path, required=True)
    parser.add_argument('--cutoff', type=float, default=0)
    parser.add_argument('--config', type=Path)
    args = parser.parse_args()
    args.output.mkdir(parents=True, exist_ok=True)
    with args.csv.open() as source:
        raw = [(row['arrival_sequence'], int(row['decode_complete_us']),
                float(row['source_rate_hz'])) for row in csv.DictReader(source)
               if row['presented'] == '1']
    epoch = min(row[1] for row in raw)
    metadata = {sequence: ((decode - epoch) / 1e6, fps)
                for sequence, decode, fps in raw}
    variants = {'vrr13': parameters(shared=0), 'current': parameters()}
    for credit in [125, 250, 500]:
        for overlap in [0, 333, 667, 1000]:
            variants[f'credit{credit}-overlap{overlap}'] = parameters(credit, overlap, 1000)
    for overlap in [0, 333, 667]:
        variants[f'legacy-buffer-overlap{overlap}'] = parameters(overlap=overlap)
    if args.config:
        variants = json.loads(args.config.read_text())
    (args.output / 'variants.json').write_text(json.dumps(variants, indent=2) + '\n')
    (args.output / 'scenarios.json').write_text(json.dumps({
        'config_schema': 1, 'scenarios': [
            {'name': name, 'parameters': {'controller': params}}
            for name, params in variants.items()]}, indent=2) + '\n')

    def run(item):
        name, params = item
        report = args.output / f'{name}.json'
        timeline = args.output / f'{name}.csv'
        command = ['build-tests/vrr/vrrreplay', str(args.trace),
                   '--output', str(report), '--timeline', str(timeline)]
        for key, value in params.items():
            command.extend(['--set', f'controller.{key}={value}'])
        with (args.output / f'{name}.log').open('w') as log:
            subprocess.run(command, stdout=log, stderr=subprocess.STDOUT, check=True)
        summary = json.loads(report.read_text())
        result = {'scenario': name, 'groups': score(timeline, metadata, args.cutoff),
                  'controller_replay_ready': summary['controller_replay_ready'],
                  'fidelity': summary['fidelity'],
                  'worker_saturated': summary['replay_worker_saturated']}
        (args.output / f'{name}-score.json').write_text(json.dumps(result, indent=2) + '\n')
        all_frames = result['groups']['all']
        print(name, 'latency_ms', round(all_frames['latency']['mean']/1000, 3),
              'interval_change_p99_ms', all_frames['interval_change']['p99']/1000,
              'steps_over_500us', all_frames['interval_change']['above_us']['500'], flush=True)
        return result
    with concurrent.futures.ThreadPoolExecutor(max_workers=3) as executor:
        results = list(executor.map(run, variants.items()))
    (args.output / 'scores.json').write_text(json.dumps(results, indent=2) + '\n')


if __name__ == '__main__':
    main()
