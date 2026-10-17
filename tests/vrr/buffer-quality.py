#!/usr/bin/env python3
"""Offline quality accounting. No native execution, policy tuning or app access."""
import argparse,collections,csv,hashlib,io,json,math,statistics,struct,zlib
from pathlib import Path

DEFINITIONS={
 'units':'us, except explicitly named seconds/rates/integrals',
 'quantile':'nearest rank ceil(p*N); missing distributions are null, never success',
 'jerk':'abs(I[n]-I[n-1]) on adjacent presented frame identities',
 'added_jerk':'abs((submit_interval-source_RTP_interval)[n] - same[n-1])',
 'B':'decision playout_delay_us; target ONLY buffer_before_target_us, never post-feedback current/target',
 'cycles':'directional-change extrema at H; floor(confirmed reversals/2) per continuous segment, incomplete half retained',
 'bounce_episode':'at least2 confirmations, gaps<=5s, first extreme through last confirmation',
 'thresholds':'raw,1us,T/2,T,2T; high B>=1/2/4/8/16ms and max-T; error/jerk>T and2ms',
 'time_integral':'piecewise-constant known decision state, gaps>1s excluded; B-target excess is NOT measured latency',
 'source':'90kHz RTP describes source timestamps; long source intervals alone are not faults',
 'display':'OS observations require matching submission ID, age<=100ms, uncertainty<=500us, after readiness; not optical',
 'settling':'only with explicit disturbance-end clock; first1s continuously within target+T and source-relative error<=T',
}
def distribution(a):
 if not a:return dict(n=0,min=None,mean=None,p50=None,p95=None,p99=None,max=None)
 a=sorted(a);return dict(n=len(a),min=a[0],mean=statistics.fmean(a),p50=a[math.ceil(.5*len(a))-1],p95=a[math.ceil(.95*len(a))-1],p99=a[math.ceil(.99*len(a))-1],max=a[-1])
def integer(r,key,default=0):
 try:return int(r.get(key,default) or default)
 except (ValueError,TypeError):return default
def text_bytes(path):
 b=Path(path).read_bytes()
 if not b.startswith(b'MLVRR1\n'):return b
 offset=7;out=[]
 while offset<len(b):
  if offset+4>len(b):raise ValueError('truncated chunk length')
  n=struct.unpack_from('<I',b,offset)[0];offset+=4
  if offset+n>len(b) or n<4:raise ValueError('truncated compressed chunk')
  c=b[offset:offset+n];offset+=n;raw=zlib.decompress(c[4:])
  if len(raw)!=struct.unpack_from('>I',c)[0]:raise ValueError('qCompress size mismatch')
  out.append(raw)
 return b''.join(out)
def read(path):
 b=text_bytes(path);footer=None;body=b
 marker=b.find(b'#vrr_trace_footer,')
 if marker>=0:
  tail=b[marker:].splitlines()
  if len(tail)!=1:raise ValueError('data after/duplicate trace footer')
  body=b[:marker];parts=tail[0].decode().split(',')[1:]
  footer=dict(p.split('=',1) for p in parts if '=' in p)
  for key,value in [('clean_shutdown','1'),('rows_dropped','0'),('write_failed','0'),('size_capped','0')]:
   if footer.get(key)!=value:raise ValueError('incomplete trace: '+key)
  if hashlib.sha256(body).hexdigest()!=footer.get('decoded_sha256'):raise ValueError('decoded checksum mismatch')
 reader=csv.DictReader(io.StringIO(body.decode()))
 wanted=set('arrival_sequence frame trace_schema trace_model_kind disposition rtp_timestamp rtp_valid decoder_output_us decode_complete_us pacer_arrival_us frame_receive_us frame_reassembled_us decode_submit_us decision_us simulated_decision_us decision_valid playout_delay_us simulated_playout_delay_us buffer_audit_revision presented cancelled presenter_submission_time_used presenter_submission_time_valid presenter_submission_time_us submission_boundary_us simulated_submission_us target_us simulated_target_us dequeue_us prepare_start_us prepare_end_us buffer_update_frame buffer_attributed_frame buffer_interval_error_us buffer_lateness_us input_percentile_last_input_eligible input_percentile_last_input_required_us input_percentile_recent_eligible input_percentile_recent_missed param_playout_input_percentile_tolerance_us param_playout_input_percentile_window_us param_playout_input_percentile_target_per_million submission_id_valid submission_id latch_valid latch_time_kind latch_submission_id latch_time_us present_end_us presentation_uncertainty_us'.split())
 wanted.update(['input_percentile_current_us','input_percentile_target_us'])
 rows=[{k:v for k,v in r.items() if k in wanted or k.startswith(('buffer_before_','buffer_after_')) or (i==0 and k.startswith('param_'))} for i,r in enumerate(reader)]
 if 'trace_schema' in (reader.fieldnames or []) and footer is None:raise ValueError('native trace footer missing')
 seq=[integer(r,'arrival_sequence') for r in rows]
 if len(seq)!=len(set(seq)) or sorted(seq)!=list(range(1,len(seq)+1)):raise ValueError('incomplete/duplicate arrival sequence')
 if footer and int(footer['rows_enqueued'])!=len(rows):raise ValueError('footer row count mismatch')
 return rows,footer,hashlib.sha256(b).hexdigest()
def zigzag(points,H):
 if not points:return []
 anchor=extreme=points[0];direction=0;events=[];last=points[0]
 for x in points[1:]:
  if H==0:
   delta=x[1]-last[1]
   if not delta:continue
   sign=1 if delta>0 else -1
   if direction and sign!=direction:events.append((last[0],x[0],last[1]))
   direction=sign;last=x;continue
  if not direction:
   if abs(x[1]-anchor[1])>=H:direction=1 if x[1]>anchor[1] else -1;extreme=x
  elif direction*(x[1]-extreme[1])>=0:extreme=x
  elif direction*(extreme[1]-x[1])>=H:
   events.append((extreme[0],x[0],extreme[1]));direction=-direction;extreme=x
 return events
def oscillation(points,T,within_layer=False):
 groups=[];g=[]
 for p in points:
  if g and (p[0]-g[-1][0]>1000000 or p[0]<=g[-1][0] or (within_layer and p[2]!=g[-1][2])):groups.append(g);g=[]
  g.append(p)
 if g:groups.append(g)
 duration=(points[-1][0]-points[0][0])/1e6 if len(points)>1 else 0;output={}
 for H in sorted(set([0,1,T/2,T,2*T])):
  count=cycles=halves=0;legs=[];episodes=[]
  for g in groups:
   ev=zigzag(g,H);count+=len(ev);cycles+=len(ev)//2;halves+=len(ev)%2
   legs.extend(abs(a[2]-b[2]) for a,b in zip(ev,ev[1:]));blocks=[];part=[]
   for e in ev:
    if part and e[1]-part[-1][1]>5000000:blocks.append(part);part=[]
    part.append(e)
   if part:blocks.append(part)
   for part in blocks:
    if len(part)<2:continue
    values=[p[1] for p in g if part[0][0]<=p[0]<=part[-1][1]]
    episodes.append(dict(start_us=part[0][0],end_us=part[-1][1],duration_s=(part[-1][1]-part[0][0])/1e6,reversals=len(part),cycles=len(part)//2,min_B_us=min(values),max_B_us=max(values)))
  output[str(H)]=dict(reversals=count,cycles=cycles,unfinished_half_cycles=halves,cycles_per_minute=cycles*60/duration if duration else None,swing_us=distribution(legs),episode_count=len(episodes),longest_episode=max(episodes,key=lambda e:e['duration_s']) if episodes else None)
 return dict(span_s=duration,hysteresis=output)
def episodes(points,predicate):
 result=[];current=None;total=integral=0
 for a,b in zip(points,points[1:]):
  dt=(b['at']-a['at'])/1e6
  if not (0<dt<=1 and predicate(a)):
   if current:result.append(current);current=None
   continue
  total+=dt;integral+=a['B']*dt
  if not current:current=dict(start_us=a['at'],end_us=b['at'],duration_s=0,min_B_us=a['B'],max_B_us=a['B'])
  current['end_us']=b['at'];current['duration_s']+=dt;current['min_B_us']=min(current['min_B_us'],a['B']);current['max_B_us']=max(current['max_B_us'],a['B'])
 if current:result.append(current)
 longest=max(result,key=lambda e:e['duration_s']) if result else None
 if longest:longest['right_censored']=longest['end_us']==points[-1]['at']
 return dict(total_s=total,count=len(result),longest=longest)
def rolling(events):
 queue=collections.deque();severity=0;maxn=maxsev=0
 for at,sev in events:
  queue.append((at,sev));severity+=sev
  while queue and queue[0][0]<=at-1000000:severity-=queue.popleft()[1]
  maxn=max(maxn,len(queue));maxsev=max(maxsev,severity)
 return dict(max_count_1s=maxn,max_severity_1s_us=maxsev)
def analyze(path,disturbance_end=None,evaluation_window=None):
 rows,footer,sha=read(path);rows.sort(key=lambda r:(integer(r,'arrival_sequence'),integer(r,'frame')))
 if not rows:raise ValueError('empty trace')
 fullrows=rows;full_origin=min((integer(r,'decision_us',integer(r,'simulated_decision_us')) for r in rows if integer(r,'decision_valid') or integer(r,'buffer_audit_revision')),default=0)
 if evaluation_window:
  rows=[r for r in rows if evaluation_window[0]<=(integer(r,'decision_us',integer(r,'simulated_decision_us')) or integer(r,'pacer_arrival_us'))<=evaluation_window[1]]
  if not rows:raise ValueError('empty evaluation window')
 profile=next((r for r in fullrows if integer(r,'buffer_audit_revision')==1),fullrows[0])
 T=integer(fullrows[0],'param_playout_input_percentile_tolerance_us',integer(profile,'buffer_before_tolerance_us',500))
 W=integer(fullrows[0],'param_playout_input_percentile_window_us',integer(profile,'buffer_before_window_us',120000000))
 ppm=integer(fullrows[0],'param_playout_input_percentile_target_per_million',integer(profile,'buffer_before_probability_ppm',995000))
 points=[];presented=[];audit_errors=[];lastE=0;lastM=0;causal=[];eligible=0;streak=longest=0
 actions=collections.Counter();reasons=collections.Counter();lastframe=None
 for r in rows:
  native='decision_us' in r;valid=integer(r,'decision_valid') if native else integer(r,'buffer_audit_revision')==1
  at=integer(r,'decision_us' if native else 'simulated_decision_us');B=integer(r,'playout_delay_us' if native else 'simulated_playout_delay_us')
  audit=integer(r,'buffer_audit_revision')==1
  if integer(r,'buffer_audit_revision') not in [0,1]:raise ValueError('unsupported buffer audit revision')
  target=integer(r,'buffer_before_target_us') if audit else None
  if valid and B>0:points.append(dict(at=at,frame=integer(r,'frame'),B=B,target=target,layer=integer(r,'buffer_before_layer_id') if audit else None,minimum=integer(r,'buffer_before_minimum_us',1000)))
  if audit:
   if integer(r,'buffer_before_applied_us')!=B:audit_errors.append((integer(r,'frame'),'decision B mismatch'))
   if native and valid and integer(r,'buffer_after_current_us')!=integer(r,'input_percentile_current_us'):audit_errors.append((integer(r,'frame'),'post-feedback current mismatch'))
   reasons[str(integer(r,'buffer_after_eligibility_reasons'))]+=1
   if target is not None:actions['grow' if target>B else 'drain' if target<B else 'hold']+=1
  frame=integer(r,'frame');isEligible=valid and integer(r,'input_percentile_last_input_eligible') and integer(r,'buffer_update_frame')==frame
  if isEligible:
   eligible+=1;need=integer(r,'input_percentile_last_input_required_us');proof=min(integer(r,'buffer_interval_error_us'),integer(r,'buffer_lateness_us'))
   sev=min(max(0,need-B-T),max(0,proof-T));miss=sev>0 and integer(r,'buffer_attributed_frame')==frame
   if miss:
    causal.append((at,sev));streak=streak+1 if lastframe is not None and frame==lastframe+1 else 1;longest=max(longest,streak)
   else:streak=0
   lastframe=frame
  else:streak=0;lastframe=None
  if r.get('disposition')=='presented' and not integer(r,'cancelled'):
   sub=integer(r,'presenter_submission_time_us') if integer(r,'presenter_submission_time_used') and integer(r,'presenter_submission_time_valid') else integer(r,'submission_boundary_us' if native else 'simulated_submission_us')
   if sub:presented.append((r,at,sub))
 points.sort(key=lambda p:p['at']);origin=points[0]['at'] if points else 0
 if audit_errors:raise ValueError('audit self-consistency failed: '+str(audit_errors[:8]))
 distributions={};thresholds={};frameErrors={}
 for scope,start in [('all',origin),('after_first_5s',max(origin,full_origin+5000000))]:
  m=collections.defaultdict(list)
  for p in points:
   if p['at']>=start:
    m['applied_B_us'].append(p['B'])
    if p['target'] is not None:m['decision_target_B_us'].append(p['target'])
  previous=None;pair=None
  for r,at,sub in sorted(presented,key=lambda x:integer(x[0],'frame')):
   if at<start:continue
   m['output_to_CPU_enqueue_us'].append(sub-integer(r,'decoder_output_us'))
   deadline=integer(r,'target_us' if 'target_us' in r else 'simulated_target_us')
   if deadline:m['CPU_deadline_lateness_us'].append(max(0,sub-deadline))
   for metric,a,b in [('predequeue_residence_us','dequeue_us','pacer_arrival_us'),('prepare_us','prepare_end_us','prepare_start_us'),('post_prepare_hold_us',None,'prepare_end_us')]:
    end=sub if a is None else integer(r,a);begin=integer(r,b)
    if end>=begin>0:m[metric].append(end-begin)
   if previous and integer(r,'frame')==integer(previous[0],'frame')+1 and integer(r,'rtp_valid') and integer(previous[0],'rtp_valid'):
    source=((integer(r,'rtp_timestamp')-integer(previous[0],'rtp_timestamp')+2**31)%2**32-2**31)*1e6/90000;interval=sub-previous[1]
    if source>0 and interval>0:
     error=interval-source;m['source_interval_us'].append(source);m['submission_interval_us'].append(interval);m['source_relative_error_abs_us'].append(abs(error))
     if scope=='all':frameErrors[integer(r,'frame')]=abs(error)
     if pair and pair[0]==integer(previous[0],'frame'):
      m['source_jerk_us'].append(abs(source-pair[1]));m['submission_jerk_us'].append(abs(interval-pair[2]));m['client_added_jerk_us'].append(abs(error-pair[3]))
     pair=(integer(r,'frame'),source,interval,error)
    else:pair=None
   else:pair=None
   previous=(r,sub)
  distributions[scope]={k:distribution(v) for k,v in m.items()}
  thresholds[scope]={k:{str(h):dict(count=sum(x>h for x in v),denominator=len(v),rate_percent=sum(x>h for x in v)*100/len(v),excess_severity_us=sum(max(0,x-h) for x in v)) for h in sorted(set([T,2000]))} for k,v in m.items() if ('jerk' in k or 'error_abs' in k or 'lateness' in k) and v}
 bpoints=[(p['at'],p['B'],p['layer']) for p in points];targetpoints=[(p['at'],p['target'],p['layer']) for p in points if p['target'] is not None]
 span=(points[-1]['at']-origin)/1e6 if points else 0
 plateaus={str(h):episodes(points,lambda p,h=h:p['B']>=h) for h in [1000,2000,4000,8000,16000]}
 if points:
  peak=max(x['B'] for x in points);plateaus['within_T_of_max']=episodes(points,lambda p:p['B']>=peak-T)
 above=None
 if targetpoints:
  integral=duration=0
  for a,b in zip(points,points[1:]):
   dt=(b['at']-a['at'])/1e6
   if 0<dt<=1 and a['target'] is not None:duration+=dt;integral+=max(0,a['B']-a['target'])*dt
  above=dict(strict=episodes(points,lambda p:p['target'] is not None and p['B']>p['target']),plus_T=episodes(points,lambda p:p['target'] is not None and p['B']>p['target']+T),positive_excess_integral_us_s=integral,known_duration_s=duration,mean_excess_B_us=integral/duration if duration else None)
 # Display evidence is never inferred from successful CPU submission.
 ids=collections.Counter(integer(r,'submission_id') for r,_,_ in presented if integer(r,'submission_id_valid'))
 submitted={integer(r,'submission_id'):(r,sub) for r,_,sub in presented if integer(r,'submission_id_valid') and ids[integer(r,'submission_id')]==1};events={};rejected=collections.Counter()
 rejected['ambiguous_submission_ids']=sum(n>1 for n in ids.values())
 conflicting=set()
 for r in fullrows:
  if not integer(r,'latch_valid') or integer(r,'latch_time_kind')!=2:continue
  ident=integer(r,'latch_submission_id');source=submitted.get(ident)
  if not source:rejected['unmatched_id']+=1;continue
  shown=integer(r,'latch_time_us');seen=integer(r,'present_end_us');ready=max(source[1],integer(source[0],'prepare_end_us'))
  if not (ready<=shown<=seen and seen-shown<=100000 and shown-ready<=100000 and 0<=integer(r,'presentation_uncertainty_us')<=500):rejected['invalid_or_uncertain']+=1;continue
  if ident in events and events[ident]!=shown:conflicting.add(ident)
  if ident not in events:events[ident]=shown
 for ident in conflicting:events.pop(ident,None)
 rejected['conflicting_event_ids']=len(conflicting)
 display=dict(qualified_events=len(events),submitted_ids=len(ids),unambiguous_submitted_ids=len(submitted),missing_events=len(ids)-len(events),coverage_percent=100*len(events)/len(ids) if ids else None,rejected=dict(rejected),latency_us=distribution([t-integer(submitted[k][0],'decoder_output_us') for k,t in events.items()]),scope='OS timestamp, not optical; missing is unknown, not success')
 settling=dict(disturbance_end_us=disturbance_end,settled_at_us=None,drained_to_min_at_us=None,scope='unavailable without labeled disturbance end')
 if disturbance_end is not None:
  start=None;minimumStart=None;previousAt=None
  for p in points:
   if p['at']<disturbance_end:continue
   if previousAt is not None and p['at']-previousAt>1000000:start=minimumStart=None
   previousAt=p['at']
   good=p['target'] is not None and p['B']<=p['target']+T and frameErrors.get(p['frame'],float('inf'))<=T
   start=p['at'] if good and start is None else start if good else None
   drain=p['target'] is not None and p['B']<=p['minimum']+1 and p['target']<=p['minimum']+1
   minimumStart=p['at'] if drain and minimumStart is None else minimumStart if drain else None
   if start is not None and p['at']-start>=1000000 and settling['settled_at_us'] is None:settling['settled_at_us']=start
   if minimumStart is not None and p['at']-minimumStart>=1000000 and settling['drained_to_min_at_us'] is None:settling['drained_to_min_at_us']=minimumStart
  settling['scope']='1s B<=target+T and source-relative interval error<=T; drain is1s B/target at minimum; no physical settling claim'
 if points:
  final=points[-1];post=[p for p in points if disturbance_end is not None and p['at']>=disturbance_end]
  settling['diagnostics']=dict(observed_until_us=final['at'],final_B_us=final['B'],final_target_us=final['target'],minimum_us=final['minimum'],post_disturbance_samples=len(post),minimum_target_requested_samples=sum(p['target'] is not None and p['target']<=p['minimum']+1 for p in post),minimum_target_first_request_us=next((p['at'] for p in post if p['target'] is not None and p['target']<=p['minimum']+1),None),drain_status='reached' if settling['drained_to_min_at_us'] is not None else 'target_never_requested_minimum_in_observed_tail' if post and all(p['target'] is not None and p['target']>p['minimum']+1 for p in post) else 'criterion_not_reached_in_observed_tail')
 last=next((r for r in reversed(rows) if integer(r,'decision_valid') or integer(r,'buffer_audit_revision')),rows[-1])
 E=integer(last,'input_percentile_recent_eligible');M=integer(last,'input_percentile_recent_missed')
 outcome=dict(eligible=eligible,misses=len(causal),rate_percent=len(causal)*100/eligible if eligible else None,excess_severity_us=sum(v for _,v in causal),longest_frame_contiguous_run=longest,**rolling(sorted(causal)),final_selected_window=dict(eligible=E,missed=M,coverage_percent=(E-M)*100/E if E else None))
 immutable=sorted((integer(r,'frame'),integer(r,'rtp_timestamp'),integer(r,'decoder_output_us'),integer(r,'frame_receive_us'),integer(r,'frame_reassembled_us'),integer(r,'decode_submit_us')) for r in fullrows)
 return dict(schema=1,definitions=DEFINITIONS,input=str(path),expanded_sha256=sha,immutable_frame_inputs_sha256=hashlib.sha256(json.dumps(immutable,separators=(',',':')).encode()).hexdigest(),model_kinds=sorted(set(integer(r,'trace_model_kind',-1) for r in rows)),evaluation_window_us=evaluation_window,integrity=dict(rows=len(fullrows),evaluated_rows=len(rows),footer=footer,arrival_sequence_complete=True),settings=dict(target_ppm=ppm,window_us=W,tolerance_us=T,controller_parameters_without_lifecycle={k:v for k,v in fullrows[0].items() if k.startswith('param_') and k!='param_playout_input_layer_lifecycle'}),origin_us=origin,span_s=span,dispositions=dict(collections.Counter(r['disposition'] for r in rows)),unsubmitted_frames=sum(r.get('disposition')!='presented' and not integer(r,'cancelled') for r in rows),distributions=distributions,thresholds=thresholds,causal=outcome,buffer=dict(applied=oscillation(bpoints,T),target=oscillation(targetpoints,T) if targetpoints else None,within_layer=oscillation(bpoints,T,True) if all(p['layer'] is not None for p in points) else None,plateaus=plateaus,above_target=above,decision_actions=dict(actions),post_feedback_reason_masks=dict(reasons)),display=display,settling=settling,limitations=['B-target unavailable on legacy traces: no substitution of post-feedback target','Frozen replay retains captured admission; closed loop retains declared service assumptions','No optical/input-to-photon, GPU contention, decoder backpressure or network transport prediction'])
def compare(a,b):
 gates=[]
 def gate(name,x,y,allowance=0,applicable=True):gates.append(dict(name=name,baseline=x,candidate=y,delta=None if x is None or y is None else y-x,allowance=allowance,applicable=applicable,passed=None if x is None or y is None else y<=x+allowance))
 for scope in ['all','after_first_5s']:
  for metric in ['output_to_CPU_enqueue_us','source_relative_error_abs_us','client_added_jerk_us']:
   for p in ['mean','p50','p95','p99','max']:
    x=a['distributions'][scope].get(metric,{}).get(p);y=b['distributions'][scope].get(metric,{}).get(p)
    gate(scope+'.'+metric+'.'+p,x,y,a['settings']['tolerance_us'] if metric=='output_to_CPU_enqueue_us' else 0)
 for scope in ['all','after_first_5s']:
  for metric,thresholds in a['thresholds'][scope].items():
   for threshold,values in thresholds.items():
    for key in ['count','rate_percent','excess_severity_us']:
     gate(scope+'.'+metric+'.over_'+threshold+'.'+key,values[key],b['thresholds'][scope].get(metric,{}).get(threshold,{}).get(key))
 for key in ['misses','excess_severity_us','longest_frame_contiguous_run','max_count_1s','max_severity_1s_us']:gate('causal.'+key,a['causal'][key],b['causal'][key])
 h=str(a['settings']['tolerance_us'])
 for key in ['cycles','reversals']:
  gate('B_'+key+'_T',a['buffer']['applied']['hysteresis'][h][key],b['buffer']['applied']['hysteresis'][h][key])
 def longest_duration(x):return x['duration_s'] if x else 0
 for kind in ['applied','target']:
  x=a['buffer'][kind];y=b['buffer'][kind]
  available=x is not None or y is not None
  gate(kind+'_bounce_longest_episode_s',longest_duration(x['hysteresis'][h]['longest_episode']) if x else None,longest_duration(y['hysteresis'][h]['longest_episode']) if y else None,applicable=available)
  gate(kind+'_bounce_swing_max_us',x['hysteresis'][h]['swing_us']['max'] or 0 if x else None,y['hysteresis'][h]['swing_us']['max'] or 0 if y else None,applicable=available)
 for threshold in a['buffer']['plateaus']:
  x=a['buffer']['plateaus'][threshold];y=b['buffer']['plateaus'][threshold]
  gate('plateau_'+threshold+'_total_s',x['total_s'],y['total_s'])
  gate('plateau_'+threshold+'_longest_s',longest_duration(x['longest']),longest_duration(y['longest']))
 x=a['buffer']['above_target'];y=b['buffer']['above_target']
 gate('above_target_plus_T_longest_s',longest_duration(x['plus_T']['longest']) if x else None,longest_duration(y['plus_T']['longest']) if y else None,applicable=x is not None or y is not None)
 gate('unsubmitted_frames',a['unsubmitted_frames'],b['unsubmitted_frames'])
 # Both missing is explicitly inapplicable to SOFTWARE comparison. Physical evidence remains unknown.
 display_available=a['display']['qualified_events']>0 or b['display']['qualified_events']>0
 gate('display_missing_events',a['display']['missing_events'],b['display']['missing_events'],applicable=display_available)
 for p in ['p99','max']:gate('OS_display_latency_'+p,a['display']['latency_us'][p],b['display']['latency_us'][p],a['settings']['tolerance_us'],applicable=display_available)
 for key in ['settled_at_us','drained_to_min_at_us']:
  gate(key,a['settling'][key],b['settling'][key],applicable=a['settling']['disturbance_end_us'] is not None)
 same=a['immutable_frame_inputs_sha256']==b['immutable_frame_inputs_sha256'] and a['settings']==b['settings'] and a['evaluation_window_us']==b['evaluation_window_us']
 return dict(schema=1,baseline=a,candidate=b,source_inputs_and_settings_match=same,gates=gates,all_available_gates_pass=same and all(g['passed'] is True for g in gates if g['applicable']),physical_display_evidence='available' if display_available else 'unknown: neither trace has qualified OS observations',scope='per-case software comparison; failed/unknown gates retained, no physical proof')
def main():
 p=argparse.ArgumentParser(description=__doc__);p.add_argument('baseline');p.add_argument('--candidate');p.add_argument('--output',required=True);p.add_argument('--disturbance-end-us',type=int);p.add_argument('--require-gates',action='store_true',help='Exit3 if comparison has any applicable failed/unknown gate.');p.add_argument('--window-start-us',type=int);p.add_argument('--window-end-us',type=int);args=p.parse_args()
 window=(args.window_start_us,args.window_end_us) if args.window_start_us is not None and args.window_end_us is not None else None
 a=analyze(args.baseline,args.disturbance_end_us,window);result=compare(a,analyze(args.candidate,args.disturbance_end_us,window)) if args.candidate else a
 Path(args.output).write_text(json.dumps(result,indent=2)+'\n')
 print(json.dumps(dict(output=args.output,rows=a['integrity']['rows'],span_s=a['span_s'],gates_pass=result.get('all_available_gates_pass'))))
 if args.require_gates and (not args.candidate or not result['all_available_gates_pass']):raise SystemExit(3)
if __name__=='__main__':main()
