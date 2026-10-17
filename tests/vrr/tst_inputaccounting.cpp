#include "../../app/streaming/video/ffmpeg-renderers/pacer/vrr/vrrtimingcontroller.h"
#include "../../app/streaming/video/timinggraph.h"
#include <cstdio>
#include <limits>
#include <random>
#include <utility>
using Vrr13::InputPercentileBuffer;
int fail=0;
void check(bool b,const char* msg){if(!b){std::fprintf(stderr,"FAIL: %s\n",msg);++fail;}}
void observe(InputPercentileBuffer& b,uint64_t f,int64_t req,int64_t clock,uint64_t room=500,uint64_t window=300000000){b.observe(f,1000000+f*10000,2000000+f*10000,req,true,1000,25000,999500,window,1000,250,0,500,room,clock);}
void rateRoundTrip() {
 VrrSessionConfig cfg;cfg.streamRateHz=116;cfg.displayRefreshHz=120;
 cfg.experimentalInputPercentilePpm=1;cfg.experimentalBoundedCatchup=true;
 cfg.smoothFrameTiming=false;cfg.timingOptions={4000,9995,300,500};
 auto policy=vrrTimingParametersForSession(cfg);policy.playoutDelayStartSeedUs=1000;
 // Keep the recorded raw-input outcome definition in this clock/accounting
 // comparison. Actual preventable-hitch scoring is exercised independently by
 // tst_inputpercentileintegration, including the production rate-layer path.
 policy.playoutInputHitchMissesOnly=0;policy.playoutInputStableAdaptation=0;
 VrrTimingController c(cfg,true,policy);uint64_t slot=1000000,pts=0,submitted=0,frame=0;
 const auto feed=[&](uint64_t count,uint64_t period,uint32_t ticks,uint64_t spike) {
  for(uint64_t n=0;n<count;++n,++frame) {
   slot+=period;pts+=ticks;const uint64_t ready=slot+1000+(frame%20==0?spike:0);
   PacedFrame f(nullptr,int(frame),uint32_t(pts),true,ready);
   f.setDeliveryTimeline(ready-800,ready-600,ready-500);
   const auto d=c.schedule(f,std::max(ready,submitted));
   const uint64_t prepared=std::max(ready,d.renderStartUs)+500;
   submitted=std::max(prepared,d.targetUs);c.notePreparationDuration(500,0,prepared);
   c.noteSchedulerDelays(0,0,true);c.noteSubmission(true,false,submitted,submitted);
  }
 };
 feed(3000,8621,776,5000);const auto fast=c.intervalStats().inputPercentile;
 feed(600,33333,3000,1000);const auto slow=c.intervalStats().inputPercentile;
 feed(600,8621,776,5000);const auto back=c.intervalStats().inputPercentile;
 const uint64_t misses=back.missed-slow.missed,eligible=back.eligible-slow.eligible;
 std::printf("bounded rate round trip: fast %llu slow %llu back %llu, misses %llu/%llu\n",(unsigned long long)fast.currentUs,(unsigned long long)slow.currentUs,(unsigned long long)back.currentUs,(unsigned long long)misses,(unsigned long long)eligible);
 check(eligible>400&&misses*2000<=eligible,"bounded rate return retains 99.95% input coverage without relearning hitches");
 check(slow.currentUs<=fast.currentUs+250,"slower rate invents no additional standing protection");
 check(back.holdRemainingUs==0,"rate transitions introduce no permanent HOLD");
}
struct DriftRun{double meanUs;uint64_t missed,eligible;};
DriftRun driftRun(bool correct,double ppm,bool sparseEarly=false) {
 VrrSessionConfig cfg;cfg.streamRateHz=116;cfg.displayRefreshHz=120;
 cfg.experimentalInputPercentilePpm=1;cfg.experimentalBoundedCatchup=true;cfg.correctInputAccounting=correct;
 cfg.smoothFrameTiming=false;cfg.timingOptions={4000,9995,300,500};
 auto policy=vrrTimingParametersForSession(cfg);policy.playoutDelayStartSeedUs=1000;
 // Keep the recorded raw-input outcome definition in this clock/accounting
 // comparison. Actual preventable-hitch scoring is exercised independently by
 // tst_inputpercentileintegration, including the production rate-layer path.
 policy.playoutInputHitchMissesOnly=0;policy.playoutInputStableAdaptation=0;
 VrrTimingController c(cfg,true,policy);double slot=1000000;uint64_t submitted=0;uint32_t pts=0;
 std::mt19937_64 rng(42);std::gamma_distribution<double> jitter(2.0,400.0);std::uniform_real_distribution<double> spike(0,1);
 double sum=0;uint64_t counted=0;
 for(uint64_t frame=0;frame<42000;++frame) {
  slot+=8600.0*(1+ppm*1e-6);pts+=774;
  // Sparse early arrivals set the windowed offset minimum, so the applied
  // offset holds and then slews in bursts instead of following drift smoothly.
  const bool early=sparseEarly&&spike(rng)<0.01;
  const uint64_t ready=early?uint64_t(slot)+300:
   uint64_t(slot)+(sparseEarly?1800:800)+uint64_t(jitter(rng))+(spike(rng)<0.003?4000:0);
  PacedFrame f(nullptr,int(frame),pts,true,ready);f.setDeliveryTimeline(ready-800,ready-600,ready-500);
  const auto d=c.schedule(f,std::max(ready,submitted));
  const uint64_t prepared=std::max(ready,d.renderStartUs)+500;
  submitted=std::max(prepared,d.targetUs);c.notePreparationDuration(500,0,prepared);
  c.noteSchedulerDelays(0,0,true);c.noteSubmission(true,false,submitted,submitted);
  if(frame>=7000){sum+=c.intervalStats().inputPercentile.currentUs;++counted;}
 }
 const auto s=c.intervalStats().inputPercentile;return {sum/counted,s.missed,s.eligible};
}
void clockDrift() {
 for(double ppm:{-50.0,50.0}) {
  const auto legacy=driftRun(false,ppm),fixed=driftRun(true,ppm);
  std::printf("clock drift %+.0f ppm: legacy %.0f us %llu/%llu misses, corrected %.0f us %llu/%llu misses\n",ppm,legacy.meanUs,(unsigned long long)legacy.missed,(unsigned long long)legacy.eligible,fixed.meanUs,(unsigned long long)fixed.missed,(unsigned long long)fixed.eligible);
  check(fixed.meanUs<=legacy.meanUs*1.25,"host/client clock drift does not ratchet retained protection");
  check(fixed.missed<=legacy.missed*2+10,"host/client clock drift does not erode retained protection");
 }
 for(double ppm:{-30.0,30.0}) {
  const auto legacy=driftRun(false,ppm,true),fixed=driftRun(true,ppm,true);
  std::printf("bursty offset %+.0f ppm: legacy %.0f us %llu/%llu misses, corrected %.0f us %llu/%llu misses\n",ppm,legacy.meanUs,(unsigned long long)legacy.missed,(unsigned long long)legacy.eligible,fixed.meanUs,(unsigned long long)fixed.missed,(unsigned long long)fixed.eligible);
  check(fixed.meanUs<=legacy.meanUs*1.1,"drift under a bursty offset does not ratchet retained protection");
  check(fixed.missed<=legacy.missed+legacy.missed/2+5,"drift under a bursty offset does not erode retained protection");
 }
}
struct DropRun{double latencyUs;uint64_t missed,eligible;};
// 116 FPS where one frame in ten arrives 8 ms before the rest, so the mapping
// minimum sits below the typical frame and the buffer covers the gap. The
// source then drops abruptly to 30 FPS without early frames: the mapping slews
// 8 ms later over seconds and the evidence needs 8 ms less buffer.
DropRun rateDropRun(bool follow,uint64_t version=6) {
 VrrSessionConfig cfg;cfg.streamRateHz=116;cfg.displayRefreshHz=120;
 cfg.experimentalInputPercentilePpm=1;cfg.experimentalBoundedCatchup=true;
 cfg.smoothFrameTiming=false;cfg.timingOptions={4000,9995,300,500};
 auto policy=vrrTimingParametersForSession(cfg);policy.playoutDelayStartSeedUs=1000;
 policy.playoutInputHitchMissesOnly=version;policy.playoutInputStableAdaptation=version>=10;
 policy.playoutInputRateDropSnap=follow;policy.playoutInputBoundedRecoveryCredit=1;
 VrrTimingController c(cfg,true,policy);uint64_t slot=1000000,submitted=0,frame=0;uint32_t pts=0;
 std::mt19937_64 rng(7);std::uniform_int_distribution<uint64_t> jitter(0,600);
 double latency=0;uint64_t counted=0,previousBuffer=0;
 const auto feed=[&](uint64_t count,uint64_t period,uint32_t ticks,bool earlyFrames,bool measure) {
  for(uint64_t n=0;n<count;++n,++frame) {
   slot+=period;pts+=ticks;
   const uint64_t ready=slot+(earlyFrames&&frame%10==0?1000:9000)+jitter(rng);
   PacedFrame f(nullptr,int(frame),pts,true,ready);f.setDeliveryTimeline(ready-800,ready-600,ready-500);
   const auto d=c.schedule(f,std::max(ready,submitted));
   if(version>=9 && previousBuffer>d.playoutDelayUs)
    check(previousBuffer-d.playoutDelayUs<=500,"actual layer drain never removes more than T in one frame");
   previousBuffer=d.playoutDelayUs;
   const uint64_t prepared=std::max(ready,d.renderStartUs)+500;
   submitted=std::max(prepared,d.targetUs);c.notePreparationDuration(500,0,prepared);
   c.noteSchedulerDelays(0,0,true);c.noteSubmission(true,false,submitted,submitted);
   if(measure){latency+=double(submitted-ready);++counted;}
  }
 };
 feed(3000,8621,776,true,false);
 const auto before=c.intervalStats().inputPercentile;
 feed(450,33333,3000,false,true);
 const auto after=c.intervalStats().inputPercentile;
 return {latency/counted,after.missed-before.missed,after.eligible-before.eligible};
}
void rateDropFollow() {
 const auto drain=rateDropRun(false),follow=rateDropRun(true);
 std::printf("116->30 drop, first 15 s: drain %.0f us ready-to-present %llu/%llu misses, follow %.0f us %llu/%llu misses\n",drain.latencyUs,(unsigned long long)drain.missed,(unsigned long long)drain.eligible,follow.latencyUs,(unsigned long long)follow.missed,(unsigned long long)follow.eligible);
 check(follow.latencyUs+3000<=drain.latencyUs,"an abrupt rate drop gives back the mapping's movement at once");
 check(follow.missed<=drain.missed+2,"following the target through a rate drop keeps coverage");
 const auto fast=rateDropRun(false,7);
 check(fast.latencyUs<=follow.latencyUs+500 && fast.missed<=follow.missed+2,
       "fast layers and gradual release avoid old transition retention even without the snap shortcut");
 const auto confirmed=rateDropRun(true,8);
 check(confirmed.latencyUs<=follow.latencyUs+500 && confirmed.missed<=follow.missed+2,
       "confirmed layers keep prompt low-rate recovery without repeated target snapping");
 const auto toleranceLimited=rateDropRun(true,9);
 std::printf("T-limited 116->30: %.0f us ready-to-present %llu/%llu misses; snap reference %.0f us %llu/%llu\n",
       toleranceLimited.latencyUs,(unsigned long long)toleranceLimited.missed,(unsigned long long)toleranceLimited.eligible,
       follow.latencyUs,(unsigned long long)follow.missed,(unsigned long long)follow.eligible);
 check(toleranceLimited.latencyUs<=follow.latencyUs+500 && toleranceLimited.missed<=follow.missed+2,
       "T-per-frame layer draining retains prompt low-rate recovery without an instant downward step");
}
// End-to-end wiring regression: the packet/RTP clock is constant while
// decoder service changes and actual source cadence moves within the same
// cached band. Mode4 must not convert that service phase into oscillator drift.
void decoderPhaseIsNotClockDrift() {
 uint64_t legacySpan=0,receiveSpan=0;
 for(uint64_t revision : {3u,4u}) {
  VrrSessionConfig cfg;cfg.streamRateHz=116;cfg.displayRefreshHz=120;
  cfg.latencyMode=1;cfg.experimentalInputPercentilePpm=1;
  cfg.experimentalBoundedCatchup=true;cfg.smoothFrameTiming=false;
  cfg.timingOptions={1000,9950,120,500};
  auto policy=vrrTimingParametersForSession(cfg);policy.playoutInputClockRelativeHistory=revision;
  VrrTimingController c(cfg,true,policy);
  uint64_t ticks=0,submitted=0;int64_t lo=std::numeric_limits<int64_t>::max();
  int64_t hi=std::numeric_limits<int64_t>::min();
  for(int frame=1;frame<=18000;++frame) {
   const uint64_t sourceUs=1000000+ticks*100/9;
   const uint64_t seconds=(sourceUs-1000000)/1000000;
   ticks+=seconds<60?776:900;
   const uint64_t slot=1000000+ticks*100/9,receive=slot+2000;
   const uint64_t phase=seconds>=20&&seconds<60?4000:0;
   const uint64_t complete=receive+400+2000+phase;
   PacedFrame f(nullptr,frame,uint32_t(ticks),true,receive+600);
   f.setDeliveryTimeline(receive,receive+200,receive+400);
   f.noteGpuReadyUs(complete);f.noteDecodeSyncWaitUs(complete-(receive+600));
   const auto d=c.schedule(f,std::max(complete,submitted));
   const uint64_t prepared=std::max(complete,d.renderStartUs)+500;
   submitted=std::max(prepared,d.targetUs);
   c.notePreparationDuration(500,0,prepared);c.noteSchedulerDelays(0,0,true);
   c.noteSubmission(true,false,submitted,submitted);
   if(seconds>=35&&seconds<=110) {
    const int64_t relative=c.intervalStats().inputPercentile.clockOffsetUs-c.playoutOffsetUs();
    lo=std::min(lo,relative);hi=std::max(hi,relative);
   }
  }
  if(revision==3)legacySpan=uint64_t(hi-lo);else receiveSpan=uint64_t(hi-lo);
 }
 check(legacySpan>1000,"legacy drift fit mistakes a decoder phase for clock motion");
 check(receiveSpan<=5,"receive clock preserves the packet clock across decoder phase and116->100 cadence");
}

int main(){
 VrrSessionConfig cfg;cfg.streamRateHz=76;cfg.displayRefreshHz=120;cfg.latencyMode=0;cfg.experimentalInputPercentilePpm=1;cfg.experimentalBoundedCatchup=true;cfg.timingOptions={4000,9995,300,500};
 auto p=vrrTimingParametersForSession(cfg);VrrTimingController c(cfg,true,p);
 check(p.playoutInputHitchMissesOnly==10,"new sessions use fast rate layers while historical clock comparisons remain explicit");
 check(p.playoutInputClockRelativeHistory==4&&p.playoutInputBoundedRecoveryCredit==0&&p.playoutInputRateDropSnap==1&&
       p.playoutInputLayerSeedRatioPerMille==1500&&p.playoutInputLearningTrim==1,"current main enables corrected accounting with physical recovery credit");
 check(p.playoutInputRecoveryCreditRoomPerMille==50&&c.recoveryHeadroomUs()>4800&&
       c.inputRecoveryCreditUs()+1>=c.recoveryHeadroomUs()/20&&c.inputRecoveryCreditUs()<=c.recoveryHeadroomUs()/20+1,"Smooth credits a twentieth of the physical VRR room");
 for(const auto& [target,share]:{std::pair<int,uint64_t>{9900,1000},{9950,500},{9995,50},{9000,1000}}){
  VrrSessionConfig scaled=cfg;scaled.timingOptions.targetHundredths=target;
  check(vrrTimingParametersForSession(scaled).playoutInputRecoveryCreditRoomPerMille==share,"credit scales with the profile's miss allowance");
 }
 auto bounded=p;bounded.playoutInputBoundedRecoveryCredit=1;VrrTimingController cb(cfg,true,bounded);
 check(cb.recoveryHeadroomUs()>4800&&cb.inputRecoveryCreditUs()==500,"bounded credit grants only the configured recovery allowance");
 cfg.correctInputAccounting=false;auto old=vrrTimingParametersForSession(cfg);VrrTimingController legacy(cfg,true,old);check(legacy.inputRecoveryCreditUs()==legacy.recoveryHeadroomUs(),"diagnostic legacy control retains original credit");
 check(p.playoutInputPercentileTargetPerMillion==999500&&p.playoutInputPercentileWindowUs==300000000&&p.playoutInputPercentileToleranceUs==500,"active 99.95/300s/0.50ms profile preserved");
 cfg.correctInputAccounting=true;cfg.streamRateHz=116;VrrTimingController high(cfg,true,vrrTimingParametersForSession(cfg));check(high.inputRecoveryCreditUs()<=high.recoveryHeadroomUs()&&high.inputRecoveryCreditUs()<500,"near-ceiling physical room retained without over-credit");
 InputPercentileBuffer a,b;a.reset(1000);b.reset(1000);
 for(uint64_t f=1;f<=2500;++f){observe(a,f,5000,100000);observe(b,f,5000,7000000);}check(a.stats().targetUs==4000&&a.stats().targetUs==b.stats().targetUs,"clock origin invariant, tolerance and recovery credit subtracted once");
 const auto historic=a.stats().missed;
 a.advance(26010000,1000,25000,999500,300000000,1000,250,0,500,500,102000);check(a.stats().targetUs==2000&&a.stats().missed==historic,"later mapping removes stale demand, preserves historical outcomes");
 a.advance(26020000,1000,25000,999500,300000000,1000,250,0,500,500,99000);check(a.stats().targetUs==5000,"earlier mapping retains genuinely increased deadline requirement");
 InputPercentileBuffer burst;burst.reset(1000);for(uint64_t f=1;f<=4500;++f)observe(burst,f,f<1000&&f%20<3?5000:0,500000,500,10000000);check(burst.stats().targetUs==1000&&burst.stats().currentUs==1000&&burst.stats().holdRemainingUs==0,"burst expires and drains without permanent HOLD");
 InputPercentileBuffer signs;signs.reset(1000);observe(signs,1,-1000,100000);check(signs.stats().missed==0&&signs.stats().targetUs==1000,"negative normalized readiness invents no demand");observe(signs,2,2000,100000);check(signs.stats().missed==0,"exact B+T+credit equality is covered");observe(signs,3,2001,100000);check(signs.stats().missed==1,"strict boundary counts one microsecond excess");
 InputPercentileBuffer extremes;extremes.reset(1000);observe(extremes,1,std::numeric_limits<int64_t>::max()-5,100);check(extremes.stats().currentUs<=25000&&extremes.stats().clipped,"signed source-coordinate addition saturates at policy cap");extremes.advance(1020000,1000,25000,999500,300000000,1000,250,0,500,500,std::numeric_limits<int64_t>::min());check(extremes.stats().targetUs==25000,"signed projection saturates safely");
 Overlay::TimingGraphPoint g;g.sourceTimingValid=true;g.sourceTimeUs=100000;g.bufferUs=1000;g.inputPercentileEnabled=true;g.producerEligible=true;g.inputToleranceUs=500;g.inputVrrHeadroomUs=500;g.producerRequiredUs=-1000;auto graph=Overlay::bufferGraphSample(g);check(graph.producerUs==-1500&&!graph.producerMissed()&&graph.bufferUs==1000,"signed graph readiness remains separate from standing buffer");
 decoderPhaseIsNotClockDrift();
 rateRoundTrip();
 clockDrift();
 rateDropFollow();
 std::printf("main input accounting: %d failures\n",fail);return fail?1:0;
}
