#!/usr/bin/env python3
"""Hardware-free repeatability, audit parity, exactness and integrity verification.

Run under nice+15. Inputs are expanded CSV; complete native captures retain their
footer. This verifies instrumentation/reconstruction, never physical quality.
"""
import argparse,csv,hashlib,importlib.util,io,json,os,subprocess,time
from pathlib import Path
HERE=Path(__file__).resolve().parent
spec=importlib.util.spec_from_file_location('quality',HERE/'buffer-quality.py')
quality=importlib.util.module_from_spec(spec);spec.loader.exec_module(quality)

def digest_projection(path):
 body=quality.text_bytes(path).split(b'#vrr_trace_footer,')[0]
 reader=csv.DictReader(io.StringIO(body.decode()));h=hashlib.sha256()
 keys=[k for k in reader.fieldnames if k!='buffer_audit_revision' and not k.startswith(('buffer_before_','buffer_after_'))]
 for row in reader:h.update(json.dumps([row[k] for k in keys],separators=(',',':')).encode());h.update(b'\n')
 return h.hexdigest()

def mutate(path,out,field=None,stale=False,dropped=False):
 """Stream rows while re-signing the body; semantic failures cannot hide behind SHA."""
 raw=quality.text_bytes(path);marker=raw.index(b'#vrr_trace_footer,');body=raw[:marker]
 footer=dict(p.split('=',1) for p in raw[marker:].decode().strip().split(',')[1:] if '=' in p)
 reader=csv.DictReader(io.StringIO(body.decode()));sha=hashlib.sha256();changed=False
 with out.open('wb') as f:
  buf=io.StringIO();writer=csv.DictWriter(buf,reader.fieldnames,lineterminator='\n')
  def flush():
   b=buf.getvalue().encode();sha.update(b);f.write(b);buf.seek(0);buf.truncate(0)
  writer.writeheader();flush()
  for row in reader:
   if field and not changed and row.get('decision_valid')=='1' and (not stale or row.get('disposition')=='stale'):
    row[field]=str(int(row[field])+1);changed=True
   writer.writerow(row);flush()
  footer['decoded_sha256']=sha.hexdigest()
  if dropped:footer['rows_dropped']='1'
  f.write(('#vrr_trace_footer,'+','.join(k+'='+v for k,v in footer.items())+'\n').encode())
 if field and not changed:raise ValueError('no qualifying row for '+field)

def run(args):
 out=Path(args.output_dir).resolve();out.mkdir(parents=True,exist_ok=True)
 source=Path(args.input).resolve();worker=Path(args.worker_replay).resolve();replay=Path(args.replay).resolve()
 if list(out.iterdir()):raise ValueError('output directory must be empty; preserve prior verification evidence')
 env=os.environ.copy()
 for key in list(env):
  if key.startswith('MOONLIGHT_'):env.pop(key)
 env['QT_QPA_PLATFORM']='offscreen';commands=[];checks={}
 (out/'predeclared.json').write_text(json.dumps(dict(input_sha256=hashlib.sha256(source.read_bytes()).hexdigest(),worker_sha256=hashlib.sha256(worker.read_bytes()).hexdigest(),replay_sha256=hashlib.sha256(replay.read_bytes()).hexdigest(),verifier_sha256=hashlib.sha256(Path(__file__).read_bytes()).hexdigest(),lifecycle=args.lifecycle,allow_missing_costs=args.allow_missing_costs,scope=__doc__),indent=2)+'\n')
 def call(name,command,expected_success=True):
  start=time.monotonic()
  with (out/(name+'.log')).open('w') as log:r=subprocess.run([str(x) for x in command],env=env,stdout=log,stderr=subprocess.STDOUT,timeout=120)
  commands.append(dict(name=name,command=[str(x) for x in command],exit=r.returncode,expected_success=expected_success,passed=(r.returncode==0)==expected_success,wall_s=time.monotonic()-start))
  (out/'commands.json').write_text(json.dumps(commands,indent=2)+'\n')
  print(name,r.returncode,flush=True)
 extra=['--allow-missing-costs'] if args.allow_missing_costs else []
 traces=[]
 for label,audit in [('on1',1),('on2',1),('off',0)]:
  trace=out/(label+'.vrrtrace');traces.append(trace)
  call(label,[worker,source,'--trace',trace,'--output',out/(label+'-model.json'),'--lifecycle',args.lifecycle,'--audit',audit]+extra)
  quality.read(trace) # Complete clean footer, SHA and contiguous unique arrivals.
 checks['audit_on_repeatable']=quality.text_bytes(traces[0])==quality.text_bytes(traces[1])
 checks['all_existing_fields_on_off_identical']=digest_projection(traces[0])==digest_projection(traces[2])
 call('exact',[replay,traces[0],'--require-exact-baseline','--jobs','1','--output',out/'exact.json'])
 for name,field,stale in [('changed-audit','buffer_before_target_us',False),('changed-stale-audit','buffer_after_feedback_frame',True)]:
  if stale and not any(r.get('disposition')=='stale' for r in quality.read(traces[0])[0]):
   checks[name]='not applicable: input has no stale decision';continue
  path=out/(name+'.csv');mutate(traces[0],path,field,stale)
  call(name,[replay,path,'--require-exact-baseline','--jobs','1','--output',out/(name+'.json')],False)
  f=json.loads((out/(name+'.json')).read_text())['fidelity'];checks[name+'-semantic-mismatch-detected']=f['buffer_audit_mismatched_rows']>0 and bool(f['buffer_audit_first_mismatches'])
 path=out/'dropped.csv';mutate(traces[0],path,dropped=True)
 call('worker-reject-dropped',[worker,path,'--trace',out/'rejected.vrrtrace','--output',out/'rejected-model.json']+extra,False)
 call('exact-reject-dropped',[replay,path,'--require-exact-baseline','--jobs','1','--output',out/'dropped.json'],False)
 passed=all(c['passed'] for c in commands) and all(v is not False for v in checks.values())
 result=dict(passed=passed,checks=checks,commands=commands,physical_display_proof=False)
 (out/'verification.json').write_text(json.dumps(result,indent=2)+'\n')
 if not passed:raise SystemExit(1)

if __name__=='__main__':
 p=argparse.ArgumentParser(description=__doc__);p.add_argument('input');p.add_argument('--worker-replay',required=True);p.add_argument('--replay',required=True);p.add_argument('--output-dir',required=True);p.add_argument('--lifecycle',choices=['captured','0','1','2'],default='captured');p.add_argument('--allow-missing-costs',action='store_true');run(p.parse_args())
