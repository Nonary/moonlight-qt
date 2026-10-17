#!/usr/bin/env python3
"""Repeatable declared-input production-worker comparison. Run under nice+15."""
import argparse,csv,hashlib,importlib.util,json,os,subprocess,time
from pathlib import Path
HERE=Path(__file__).resolve().parent
spec=importlib.util.spec_from_file_location('quality',HERE/'buffer-quality.py');quality=importlib.util.module_from_spec(spec);spec.loader.exec_module(quality)
def generate(case,plan,parameters,path):
 p={**parameters,'playout_input_percentile_target_per_million':plan['session']['target_ppm'],'playout_input_percentile_window_us':plan['session']['window_us'],'playout_input_percentile_tolerance_us':plan['session']['tolerance_us'],'playout_delay_start_seed_us':0}
 keys=['arrival_sequence','frame','rtp_timestamp','rtp_valid','pacer_arrival_us','decoder_output_us','decode_complete_us','model_decode_observed','frame_receive_us','frame_reassembled_us','decode_submit_us','prepare_us','present_call_us','cancelled','display_refresh_hz','stream_rate_hz','can_latch_present','session_latency_mode']+['param_'+k for k in p]
 seed=plan['seed'];ticks=0.0;frame=0;t=0;gap=False
 with path.open('w',newline='') as f:
  w=csv.DictWriter(f,keys);w.writeheader()
  while t<case['duration_s']:
   fps=plan['session']['stream_hz']
   if case['kind']=='rates' and 50<=t<100:fps=72
   if case['kind']=='expiry' and 155<=t<162:fps=28
   if case['kind']=='source-gap' and t>=75 and not gap:ticks+=4*90000;gap=True
   ticks+=90000/fps;t=round(ticks)/90000;frame+=1;slot=1000000+round(ticks*100/9)
   seed=(1664525*seed+1013904223)&0xffffffff;delay=1800+seed%201
   if case['kind'] in ['delivery','drain'] and 40<=t<60 and frame%79 in [0,1]:delay+=6100 if frame%79==0 else 3400
   if case['kind']=='expiry' and ((8<t<18) or (163<t<174)) and frame%19==0:delay+=5900
   cancelled=case['kind']=='expiry' and 30<=t<155
   prepare=350
   if case['kind']=='render' and 40<=t<70 and frame%113==0:prepare=65000
   output=slot+delay;ready=output+(11000 if case['kind']=='decode' and 40<=t<70 and frame%83==0 else 400 if case['kind']=='decode' else 0)
   r=dict(arrival_sequence=frame,frame=frame,rtp_timestamp=round(ticks)&0xffffffff,rtp_valid=1,pacer_arrival_us=output,decoder_output_us=output,decode_complete_us=ready,model_decode_observed=1,frame_receive_us=output-1100,frame_reassembled_us=output-700,decode_submit_us=output-500,prepare_us=prepare,present_call_us=40,cancelled=int(cancelled),display_refresh_hz=plan['session']['display_hz'],stream_rate_hz=plan['session']['stream_hz'],can_latch_present=1,session_latency_mode=1)
   r.update({'param_'+k:v for k,v in p.items()});w.writerow(r)
 return hashlib.sha256(path.read_bytes()).hexdigest()
def run(args):
 O=Path(args.output_dir).resolve();O.mkdir(parents=True,exist_ok=True)
 if any(O.iterdir()):raise ValueError('output directory must be empty; preserve prior results')
 plan=json.loads((HERE/'configs/buffer-quality-validation.json').read_text());parameters=json.loads(Path(args.parameters).read_text())
 if 'controller' in parameters:parameters=parameters['controller']
 # This file is written BEFORE any case runs, including all cost/gate choices.
 frozen=dict(plan=plan,parameters=parameters,worker_sha256=hashlib.sha256(Path(args.worker_replay).read_bytes()).hexdigest(),generator_sha256=hashlib.sha256(Path(__file__).read_bytes()).hexdigest(),reporter_sha256=hashlib.sha256((HERE/'buffer-quality.py').read_bytes()).hexdigest())
 (O/'reporter-used.py').write_bytes((HERE/'buffer-quality.py').read_bytes())
 (O/'generator-used.py').write_bytes(Path(__file__).read_bytes())
 (O/'predeclared.json').write_text(json.dumps(frozen,indent=2)+'\n')
 env=os.environ.copy()
 for k in list(env):
  if k.startswith('MOONLIGHT_'):env.pop(k)
 env['QT_QPA_PLATFORM']='offscreen';results=[]
 cases=[]
 for case in plan['cases']:
  source=O/(case['name']+'-input.csv');digest=generate(case,plan,parameters,source);cases.append((case['name'],source,digest,case.get('disturbance_end_us'),None))
 if args.capture_root:
  fixtures=json.loads((HERE/'fixtures/buffer-bouncing-episodes.json').read_text())
  for fixture in fixtures['captures']:
   source=Path(args.capture_root)/fixture['basename']
   if hashlib.sha256(source.read_bytes()).hexdigest()!=fixture['sha256']:raise ValueError('capture identity mismatch')
   expanded=O/(source.name+'.csv');expanded.write_bytes(quality.text_bytes(source));cases.append((fixture['name'],expanded,fixture['sha256'],None,(fixture['episode_start_us'],fixture['episode_end_us'])))
 for name,source,digest,end,window in cases:
  summaries={};tracepaths={};model_hashes=[]
  for mode,label in [(0,'control'),(2,'lifecycle')]:
   trace=O/(name+'-'+label+'.vrrtrace');meta=O/(name+'-'+label+'-model.json')
   command=[str(Path(args.worker_replay).resolve()),str(source),'--trace',str(trace),'--output',str(meta),'--lifecycle',str(mode),'--allow-missing-costs']
   with (O/(name+'-'+label+'.log')).open('w') as log:subprocess.run(command,env=env,stdout=log,stderr=subprocess.STDOUT,check=True,timeout=90)
   model_hashes.append(json.loads(meta.read_text())['source_input_sha256'])
   summaries[label]=quality.analyze(trace,end,window);tracepaths[label]=trace
   (O/(name+'-'+label+'-quality.json')).write_text(json.dumps(summaries[label],indent=2)+'\n')
  assert len(set(model_hashes))==1 and model_hashes[0]==hashlib.sha256(source.read_bytes()).hexdigest(),'worker immutable input identity mismatch'
  comparison=quality.compare(summaries['control'],summaries['lifecycle']);(O/(name+'-comparison.json')).write_text(json.dumps(comparison,indent=2)+'\n')
  results.append(dict(case=name,input_sha256=digest,worker_input_sha256=model_hashes[0],control=tracepaths['control'].name,candidate=tracepaths['lifecycle'].name,all_gates_pass=comparison['all_available_gates_pass'],failed_or_unknown_gates=[g for g in comparison['gates'] if g['applicable'] and g['passed'] is not True],inapplicable_gates=[g['name'] for g in comparison['gates'] if not g['applicable']]))
  (O/'results.json').write_text(json.dumps(results,indent=2)+'\n');print(name,'passed' if comparison['all_available_gates_pass'] else 'TRADEOFF/FAIL',flush=True)
 # Human-readable report includes EVERY failed gate and all requested tails.
 text=['Production-worker closed-loop software comparison. No native display/GPU/network prediction.','Settings/gates frozen before execution: predeclared.json. Captured episodes are consumed regression inputs, not unseen validation.']
 rows=[]
 for result in results:
  text += ['',result['case']+' | '+('PASS' if result['all_gates_pass'] else 'TRADEOFF/FAIL')]
  for label in ['control','lifecycle']:
   a=json.loads((O/(result['case']+'-'+label+'-quality.json')).read_text())
   text.append(label+' | causal='+json.dumps(a['causal'])+' | B_cycles='+json.dumps(a['buffer']['applied']['hysteresis'][str(a['settings']['tolerance_us'])]))
   for scope,metrics in a['distributions'].items():
    for metric,d in metrics.items():
     rows.append(dict(case=result['case'],variant=label,scope=scope,metric=metric,**d));text.append(scope+' | '+metric+' | '+json.dumps(d))
  for g in result['failed_or_unknown_gates']:text.append('FAILED/UNKNOWN '+json.dumps(g))
 (O/'REPORT.txt').write_text('\n'.join(text)+'\n')
 with (O/'statistics.csv').open('w',newline='') as f:
  w=csv.DictWriter(f,['case','variant','scope','metric','n','min','mean','p50','p95','p99','max']);w.writeheader();w.writerows(rows)
 if args.require_gates and not all(r['all_gates_pass'] for r in results):raise SystemExit(3)
if __name__=='__main__':
 p=argparse.ArgumentParser(description=__doc__);p.add_argument('--worker-replay',required=True);p.add_argument('--parameters',required=True);p.add_argument('--output-dir',required=True);p.add_argument('--capture-root');p.add_argument('--require-gates',action='store_true');run(p.parse_args())
