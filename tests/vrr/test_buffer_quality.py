import unittest,importlib.util,subprocess,sys,tempfile,csv,hashlib,io,json,struct,zlib
from pathlib import Path
spec=importlib.util.spec_from_file_location('quality',Path(__file__).with_name('buffer-quality.py'));q=importlib.util.module_from_spec(spec);spec.loader.exec_module(q)
class QualityContracts(unittest.TestCase):
 def fixture(self,rows):
  stream=io.StringIO();w=csv.DictWriter(stream,list(dict.fromkeys(k for r in rows for k in r)));w.writeheader();w.writerows(rows)
  body=stream.getvalue().encode();footer=f'#vrr_trace_footer,format_version=2,clean_shutdown=1,rows_enqueued={len(rows)},rows_dropped=0,write_failed=0,size_capped=0,decoded_sha256={hashlib.sha256(body).hexdigest()}\n'.encode()
  temp=tempfile.TemporaryDirectory();self.addCleanup(temp.cleanup);p=Path(temp.name)/'trace.csv';p.write_bytes(body+footer);return p
 def rows(self):
  return [dict(trace_schema=5,trace_model_kind=2,arrival_sequence=i+1,frame=i+1,rtp_timestamp=i*750,rtp_valid=1,decoder_output_us=1000000+i*8333,pacer_arrival_us=1000000+i*8333,decision_us=1000000+i*8333,decision_valid=1,disposition='presented',presented=1,cancelled=0,submission_boundary_us=1004000+i*8333,playout_delay_us=4000,input_percentile_current_us=6500,input_percentile_target_us=6000,buffer_audit_revision=1,buffer_before_applied_us=4000,buffer_before_target_us=3000,buffer_before_layer_id=1,buffer_after_current_us=6500,param_playout_input_percentile_tolerance_us=500) for i in range(5)]
 def test_snapshot_phase_never_substitutes_post_feedback(self):
  a=q.analyze(self.fixture(self.rows()));self.assertEqual(a['distributions']['all']['applied_B_us']['mean'],4000);self.assertEqual(a['distributions']['all']['decision_target_B_us']['mean'],3000)
  self.assertAlmostEqual(a['buffer']['above_target']['mean_excess_B_us'],1000)
 def test_legacy_target_remains_unknown(self):
  r=self.rows()
  for x in r:x['buffer_audit_revision']=0
  a=q.analyze(self.fixture(r));self.assertIsNone(a['buffer']['above_target']);self.assertNotIn('decision_target_B_us',a['distributions']['all'])
 def test_unknown_display_not_success(self):
  a=q.analyze(self.fixture(self.rows()));self.assertIsNone(a['display']['latency_us']['p99']);self.assertEqual(a['display']['qualified_events'],0)
 def test_hysteresis_and_plateau_do_not_invent_cycles(self):
  self.assertEqual(len(q.zigzag([(0,1000,1),(1,1100,1),(2,1000,1)],500)),0)
  points=[(i*1000000,v,1) for i,v in enumerate([1000,2000,1000,2000,1000])]
  a=q.oscillation(points,500)['hysteresis']['500'];self.assertEqual((a['reversals'],a['cycles'],a['unfinished_half_cycles']),(3,1,1))
 def test_source_variation_is_not_added_jerk(self):
  r=self.rows();ticks=[0,750,2250,3000,5250]
  for x,t in zip(r,ticks):x['rtp_timestamp']=t;x['submission_boundary_us']=1004000+t*100//9
  a=q.analyze(self.fixture(r));self.assertLess(a['distributions']['all']['client_added_jerk_us']['max'],1.1);self.assertGreater(a['distributions']['all']['source_jerk_us']['max'],10000)
 def test_audit_corruption_is_rejected(self):
  r=self.rows();r[1]['buffer_before_applied_us']=123
  with self.assertRaisesRegex(ValueError,'self-consistency'):q.analyze(self.fixture(r))
 def test_checksum_and_dropped_accounting_are_rejected(self):
  p=self.fixture(self.rows());p.write_bytes(p.read_bytes().replace(b'rows_dropped=0',b'rows_dropped=1'))
  with self.assertRaisesRegex(ValueError,'rows_dropped'):q.read(p)
  p=self.fixture(self.rows());p.write_bytes(p.read_bytes().replace(b'6500',b'6501',1))
  with self.assertRaisesRegex(ValueError,'checksum'):q.read(p)
 def test_missing_arrival_is_rejected(self):
  r=self.rows();r[2]['arrival_sequence']=99
  with self.assertRaisesRegex(ValueError,'arrival sequence'):q.read(self.fixture(r))
 def test_chunked_roundtrip(self):
  p=self.fixture(self.rows());body=p.read_bytes();compressed=struct.pack('>I',len(body))+zlib.compress(body);p.write_bytes(b'MLVRR1\n'+struct.pack('<I',len(compressed))+compressed)
  self.assertEqual(q.analyze(p)['integrity']['rows'],5)
 def long_rows(self):
  template=self.rows()[0];out=[]
  for i in range(840):
   r=dict(template,arrival_sequence=i+1,frame=i+1,rtp_timestamp=i*750,decoder_output_us=1000000+i*8333,pacer_arrival_us=1000000+i*8333,decision_us=1000000+i*8333,submission_boundary_us=1004000+i*8333,buffer_before_target_us=4000,input_percentile_current_us=4000,buffer_after_current_us=4000,buffer_before_minimum_us=1000)
   out.append(r)
  return out
 def comparison(self,baseline,candidate,end=None):return q.compare(q.analyze(self.fixture(baseline),end),q.analyze(self.fixture(candidate),end))
 def failed(self,result,name):
  gate=next(g for g in result['gates'] if g['name']==name);self.assertTrue(gate['applicable']);self.assertIsNot(gate['passed'],True);self.assertFalse(result['all_available_gates_pass'])
 def test_deliberate_bouncing_is_caught(self):
  a=self.long_rows();b=[dict(r) for r in a]
  for i,r in enumerate(b):r['playout_delay_us']=r['buffer_before_applied_us']=4000+(2000 if i//30%2 else 0)
  result=self.comparison(a,b);self.failed(result,'B_cycles_T');self.failed(result,'applied_bounce_longest_episode_s')
 def test_deliberate_plateau_duration_is_caught(self):
  a=self.long_rows();b=[dict(r) for r in a]
  for r in b[300:600]:r['playout_delay_us']=r['buffer_before_applied_us']=8000
  self.failed(self.comparison(a,b),'plateau_8000_longest_s')
 def test_deliberate_jerk_and_latency_tail_are_caught(self):
  a=self.long_rows();b=[dict(r) for r in a];b[700]['submission_boundary_us']+=7000
  result=self.comparison(a,b);self.failed(result,'all.client_added_jerk_us.max');self.failed(result,'after_first_5s.output_to_CPU_enqueue_us.max')
 def test_deliberate_missed_frames_are_caught(self):
  a=self.long_rows();b=[dict(r) for r in a];b[700]['disposition']='dropped_capacity'
  self.failed(self.comparison(a,b),'unsubmitted_frames')
 def test_deliberate_causal_miss_cluster_is_caught(self):
  a=self.long_rows();b=[dict(r) for r in a]
  for r in b[700:703]:r.update(input_percentile_last_input_eligible=1,buffer_update_frame=r['frame'],buffer_attributed_frame=r['frame'],input_percentile_last_input_required_us=5500,buffer_interval_error_us=1500,buffer_lateness_us=1500)
  result=self.comparison(a,b);self.failed(result,'causal.longest_frame_contiguous_run');self.failed(result,'causal.max_count_1s');self.assertEqual(result['candidate']['causal']['misses'],3)
 def display_rows(self):
  rows=self.long_rows()
  for r in rows:r.update(submission_id_valid=1,submission_id=r['frame'],latch_valid=1,latch_time_kind=2,latch_submission_id=r['frame'],latch_time_us=r['submission_boundary_us']+1000,present_end_us=r['submission_boundary_us']+1000,presentation_uncertainty_us=100)
  return rows
 def test_lost_display_coverage_is_unknown_and_fails(self):
  a=self.display_rows();b=[dict(r,latch_valid=0) for r in a];result=self.comparison(a,b)
  self.failed(result,'display_missing_events');self.failed(result,'OS_display_latency_p99');self.assertIsNone(result['candidate']['display']['latency_us']['p99'])
 def test_both_display_unknown_is_explicitly_inapplicable(self):
  a=self.long_rows();r=self.comparison(a,a);self.assertTrue(r['all_available_gates_pass']);self.assertTrue(r['physical_display_evidence'].startswith('unknown'));self.assertFalse(next(g for g in r['gates'] if g['name']=='display_missing_events')['applicable'])
 def test_ambiguous_display_id_is_excluded(self):
  r=self.display_rows();r[1]['submission_id']=r[0]['submission_id'];a=q.analyze(self.fixture(r));self.assertEqual(a['display']['rejected']['ambiguous_submission_ids'],1);self.assertEqual(a['display']['qualified_events'],838)
 def test_drain_unknown_remains_failing_even_if_both_unknown(self):
  a=self.long_rows();result=self.comparison(a,a,2000000);self.failed(result,'drained_to_min_at_us');self.assertEqual(result['baseline']['settling']['diagnostics']['drain_status'],'target_never_requested_minimum_in_observed_tail')
 def test_roi_keeps_full_integrity_and_startup_origin(self):
  r=self.long_rows();a=q.analyze(self.fixture(r),evaluation_window=(2000000,3000000));self.assertEqual(a['integrity']['rows'],840);self.assertLess(a['integrity']['evaluated_rows'],200);self.assertEqual(a['distributions']['after_first_5s'].get('applied_B_us',{'n':0})['n'],0)
 def test_unsupported_revision_and_truncated_chunk_fail(self):
  r=self.rows();r[1]['buffer_audit_revision']=99
  with self.assertRaisesRegex(ValueError,'unsupported'):q.analyze(self.fixture(r))
  p=self.fixture(self.rows());p.write_bytes(b'MLVRR1\n'+struct.pack('<I',999)+b'broken')
  with self.assertRaisesRegex(ValueError,'truncated'):q.read(p)
 def test_asynchronous_display_event_matches_submission_id(self):
  r=self.display_rows();r[2]['latch_submission_id']=r[1]['submission_id'];r[2]['latch_time_us']=r[1]['latch_time_us'];a=q.analyze(self.fixture(r));self.assertEqual(a['display']['qualified_events'],839)
 def test_conflicting_display_events_are_excluded(self):
  r=self.display_rows();r[2]['latch_submission_id']=r[1]['submission_id'];r[2]['latch_time_us']=r[1]['latch_time_us']+100;a=q.analyze(self.fixture(r));self.assertEqual(a['display']['rejected']['conflicting_event_ids'],1);self.assertEqual(a['display']['qualified_events'],838)
 def test_trailing_footer_data_rejected(self):
  p=self.fixture(self.rows());p.write_bytes(p.read_bytes()+b'ignored-data\n')
  with self.assertRaisesRegex(ValueError,'after/duplicate'):q.read(p)
 def test_roi_keeps_producer_drop_using_arrival_time(self):
  r=self.long_rows();r[200].update(decision_us=0,decision_valid=0,buffer_audit_revision=0,disposition='dropped_capacity');a=q.analyze(self.fixture(r),evaluation_window=(2000000,3000000));self.assertEqual(a['unsubmitted_frames'],1)
 def test_cli_required_gate_returns_failure_after_saving_report(self):
  a=self.long_rows();b=[dict(r) for r in a];b[700]['disposition']='dropped_capacity';ap=self.fixture(a);bp=self.fixture(b);out=ap.parent/'result.json'
  r=subprocess.run([sys.executable,str(Path(__file__).with_name('buffer-quality.py')),str(ap),'--candidate',str(bp),'--require-gates','--output',str(out)],capture_output=True,text=True)
  self.assertEqual(r.returncode,3);self.assertTrue(out.exists());self.assertFalse(json.loads(out.read_text())['all_available_gates_pass'])
 def test_timeline_profile_comes_from_audit_including_zero_tolerance(self):
  r=self.rows()
  for row in r:row.pop('param_playout_input_percentile_tolerance_us');row.update(buffer_before_tolerance_us=0,buffer_before_window_us=10000000,buffer_before_probability_ppm=999000)
  a=q.analyze(self.fixture(r));self.assertEqual((a['settings']['tolerance_us'],a['settings']['window_us'],a['settings']['target_ppm']),(0,10000000,999000))
if __name__=='__main__':unittest.main()
