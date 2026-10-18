#!/usr/bin/env python3
"""Extract the 2026-10-17 buffer regression as scrubbed numeric controller inputs.

Retains every decision from stream start; never cut away pre-roll. This fixture
contains recorded feedback, not a closed-loop worker simulation. Validate the
original trace with --require-exact-baseline before using it as evidence.
The test independently checks every retained buffer state under revision 1.
"""
import csv,json,hashlib,zlib,struct,base64,io
from pathlib import Path
import argparse
parser=argparse.ArgumentParser(description=__doc__)
parser.add_argument('trace',type=Path,help='original trace, used for provenance hash')
parser.add_argument('decoded_csv',type=Path,help='CSV from decode-vrr-trace.py')
parser.add_argument('output',type=Path)
args=parser.parse_args()
source=args.trace
expected_source_hash='272a4d9927c706d860b912d6123a8f995a1b410494d8995756d9e2cc74b8f466'
if hashlib.sha256(source.read_bytes()).hexdigest()!=expected_source_hash:
 parser.error('this named regression requires its documented original source capture')
decoded=args.decoded_csv.read_bytes()
if b'#vrr_trace_footer,' not in decoded:
 parser.error('refusing an active or incomplete capture')
body,footer=decoded.rsplit(b'#vrr_trace_footer,',1)
footer_values=dict(part.split('=',1) for part in footer.decode().strip().split(','))
if any(footer_values.get(k)!=v for k,v in {'clean_shutdown':'1','rows_dropped':'0','write_failed':'0','size_capped':'0'}.items()):
 parser.error('capture must close cleanly without dropped rows, write failure or cap')
if hashlib.sha256(body).hexdigest()!=footer_values.get('decoded_sha256'):
 parser.error('decoded CSV does not match its capture footer')
if hashlib.sha256(body).hexdigest()!='e0d9fd705603f9efb8c00f755b1cda2e384bef946243bdb4fa9f66548cf662e4':
 parser.error('decoded CSV is not the documented source capture; do not mix trace and CSV paths')
rows=[r for r in csv.DictReader(io.StringIO(decoded.decode())) if r.get('decision_valid')=='1']
keys='frame rtp_timestamp rtp_valid decoder_output_us decode_complete_us decode_sync_wait_us frame_receive_us frame_reassembled_us decode_submit_us decode_hold_us lost_packets decision_us external_rebase_applied trajectory_resync_dropped trajectory_resync_deferred input_buffer_miss_observation_us input_network_buffer_miss input_decode_buffer_miss prepare_start_us prepare_end_us prepare_us prepare_timing_valid prepare_acquire_us gpu_ready_timing_valid gpu_ready_wait_result_valid gpu_ready_wait_result gpu_ready_wait_start_us gpu_ready_time_us gpu_ready_wait_us render_wait_entry_us render_wait_overshoot_us target_scheduler_delay_us target_scheduler_delay_valid spacing_guard_feedback_us native_flip_protection_latched native_flip_protection_reference_us presented cancelled submission_boundary_us scheduling_boundary_us input_percentile_current_us input_percentile_target_us input_percentile_eligible input_percentile_missed input_percentile_samples target_us source_rate_changed phase_discontinuity source_period_us'.split()
keys += ['disposition_code']
disps={'presented':0,'output_dropped':1,'stale':2,'interrupted':3,'preparation_failed':4}
params={k.removeprefix('param_'):int(v) for k,v in rows[0].items() if k.startswith('param_')}
vals=[]
for r in rows:
 a=[int(r.get(k) or 0) if k!='disposition_code' else disps[r['disposition']] for k in keys]
 vals.append(a)
fixture={'version':1,'provenance':{'source_basename':'moonlight-pacing-20261017-171738-793163.vrrtrace','sha256':hashlib.sha256(source.read_bytes()).hexdigest(),'source_revision':'7d1148f9339546a12b80acf5ae3c606169640882','running_executable_sha256':'31c2a854ee2acec12517d3511469600053f429b229455af0a2d86938b3a71270','compiled_commit_verified':False,'clean_footer':True,'pre_roll':'Every decision from stream start. No mid-history reset or truncated pre-roll. Numeric controller inputs only; no host names, addresses, tokens, paths or user settings.','scope':'Recorded input and recorded feedback; controller-only, not alternate worker admission or physical presentation.'},'display_hz':int(rows[0]['display_refresh_hz']),'stream_hz':int(rows[0]['stream_rate_hz']),'can_latch':int(rows[0]['can_latch_present']),'additional_queue':int(rows[0]['additional_queued_frame']),'params':params,'initial_profile':json.loads(base64.b64decode(rows[0]['playout_initial_profile'])),'columns':keys,'rows':vals,'segments':[{'name':'reported-8621us-plateau','start_frame':39926,'end_frame':51181,'expected_clean_misses':0,'max_no_downward_progress_us':10000000},{'name':'falling-target-drain','start_frame':18000,'end_frame':19600},{'name':'input-jitter-growth-recovery','start_frame':3900,'end_frame':5900},{'name':'fps-transition','start_frame':7000,'end_frame':8200}]}
previous=[0]*len(keys)
for row in fixture['rows']:
 original=row[:]
 for i,value in enumerate(row):row[i]=value-previous[i]
 previous=original
fixture['row_encoding']='column-deltas-v1'
b=json.dumps(fixture,separators=(',',':')).encode();out=args.output;out.write_bytes(struct.pack('>I',len(b))+zlib.compress(b,9)); print('rows',len(vals),'decoded',len(b),'compressed',out.stat().st_size)
