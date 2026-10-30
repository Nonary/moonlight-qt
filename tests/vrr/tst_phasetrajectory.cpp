#include <vector>
#include <algorithm>
#include "../../app/streaming/video/ffmpeg-renderers/pacer/vrr/phasetrajectory.h"
#include "../../app/streaming/video/ffmpeg-renderers/pacer/vrr/vrrtimingcontroller.h"
#include "../../app/streaming/video/ffmpeg-renderers/vrrpreparedframe.h"
#include <cstdio>
#include <limits>
#include <thread>
#include <atomic>
namespace {
int failures=0;
void check(bool ok,const char* what) {if(!ok){++failures;std::fprintf(stderr,"FAIL: %s\n",what);}}
void compressionAndAcceptance() {
    Vrr13::PhaseTrajectory p;
    p.commit(1,100000,106690,106690,true,false,true);
    const auto s=p.plan(2,109719,109719,109719,109719,8333,115023,750,9719,true);
    // 6690us carry repays 1/32 (209us) on the 9719us successor: 2.2% fast.
    check(s.continuity && s.minimumIntervalUs==9510 && s.targetUs==116200,
        "6690us late predecessor is held: 9719us intended successor shown 9510us later, not compressed to 3029us");
    check(s.targetUs-106690==9510 && s.addedUs==6481 && s.carryUs==6481 && !s.resyncRecommended,
        "catchup repays 1/32 of carry per successor and reports real6481us carry");
    // Small carry repays at the 0.5% minimum: invisible, and it still converges.
    p.reset();p.commit(1,100000,100400,100400,true,false,true);
    const auto small=p.plan(2,109719,109719,109719,109719,8333,0,750,9719,true);
    check(small.minimumIntervalUs==9719-48 && small.carryUs==400-48,"small carry repays the 0.5% minimum per frame");
    // Near the refresh ceiling the panel bounds repayment: 8400us source at 120Hz has 67us of room.
    p.reset();p.commit(1,100000,106000,106000,true,false,true);
    const auto ceiling=p.plan(2,108400,108400,108400,108400,8333,0,750,8400,true);
    check(ceiling.minimumIntervalUs==8333 && ceiling.targetUs==114333,"repayment is limited to the VRR room above the fastest refresh");
    // Planning alone cannot acquire an anchor.
    Vrr13::PhaseTrajectory uncommitted;
    uncommitted.plan(1,100000,100000,100000,100000,8333,0,750,9719,true);
    check(!uncommitted.plan(2,109719,109719,109719,109719,8333,0,750,9719,true).continuity,
        "unaccepted plan never commits presentation ownership");
    // An accepted deadline 20 ms after its slot would carry two source periods;
    // carry is capped at one, so the successor takes the physical floor.
    p.reset();p.commit(1,100000,120000,101000,true,false,true);
    check(p.plan(2,109719,109719,109719,109719,8333,128333,750,9719,true).targetUs==128333 && p.stats().capped==1,
        "native early enqueue keeps the accepted anchor; carry above one source period is capped at the physical floor");
    p.reset();p.commit(1,100000,100000,120000,true,false,true);
    check(p.plan(2,109719,109719,109719,109719,8333,128333,750,9719,true).targetUs==128333 && p.stats().capped==1,
        "late native enqueue raises the anchor; capped carry yields the physical floor");
    // Every frame is submitted late by the scheduler's usual overshoot (here
    // 200/380us, median 290us) and so is its successor: that common offset must
    // not create carry. With the 250us allowance of the 19:38 capture, the old
    // raw-actual anchor ratcheted 40us per frame (~8 ms after 200 frames).
    p.reset();
    uint64_t peakCarry=0;
    for(uint64_t f=1,slot=100000,nominal=100000;f<400;++f,slot+=9719,nominal+=9719) {
        const auto planned=p.plan(f,slot,nominal,nominal,nominal,8333,0,250,9719,true);
        if(f>40) peakCarry=std::max(peakCarry,planned.carryUs);
        p.commit(f,slot,planned.targetUs,planned.targetUs+(f%2?200:380),true,false,true);
    }
    check(peakCarry==0,"usual submission overshoot above the allowance never ratchets carry");
    // Wayland mailbox: the caller reports the actual submission as both the
    // accepted and the actual time (the 20:53 capture pinned carry at one
    // source period this way). Lateness must be measured against the plan.
    p.reset();
    uint64_t mailboxPeak=0;
    for(uint64_t f=1,slot=100000;f<400;++f,slot+=9719) {
        const auto planned=p.plan(f,slot,slot,slot,slot,8333,0,750,9719,true);
        if(f>40) mailboxPeak=std::max(mailboxPeak,planned.carryUs);
        const uint64_t actual=planned.targetUs+(f%2?200:380);
        p.commit(f,slot,actual,actual,true,false,true);
    }
    check(mailboxPeak==0,"mailbox submissions reporting actual time as accepted never ratchet carry");
    // A genuinely late frame (6.69 ms beyond the usual overshoot) still creates
    // carry, spread over successors at the allowance per frame.
    auto late=p.plan(400,100000+399*9719,100000+399*9719,100000+399*9719,0,8333,0,250,9719,true);
    p.commit(400,100000+399*9719,late.targetUs,late.targetUs+290+6690,true,false,true);
    late=p.plan(401,100000+400*9719,100000+400*9719,100000+400*9719,0,8333,0,250,9719,true);
    check(late.carryUs>=6690-250-200 && late.carryUs<=6690+200,
        "lateness beyond the usual overshoot is carried, minus one drain step");
}
void bufferReleaseDoesNotCreateCarry() {
    for (bool current : {false, true}) {
        Vrr13::PhaseTrajectory trajectory;
        trajectory.setDrainAtAllowance(true, current);
        trajectory.plan(1, 100000, 101000, 101000, 101000, 8333, 0, 500, 10000, true);
        trajectory.commit(1, 100000, 101000, 101000, true, false, true);
        const auto next = trajectory.plan(2, 110000, 110999, 110999, 110999,
            8333, 0, 500, 10000, true);
        check(next.carryUs == (current ? 0u : 1u) && next.targetUs >= 109333,
            "one-microsecond buffer release stays outside recovery while recorded policy retains its carry");
    }
}
void carryRepaymentKnobs() {
    // 6690us carry before a 9719us intended successor, 750us allowance.
    const auto planWith=[](uint64_t limit) {
        Vrr13::PhaseTrajectory p;p.commit(1,100000,106690,106690,true,false,true);
        return p.plan(2,109719,109719,109719,109719,8333,115023,750,9719,true,limit);
    };
    check(planWith(0).targetUs==116200,"no carry limit keeps the 1/32 repayment");
    check(planWith(8000).targetUs==116200,"a carry limit above the carry changes nothing");
    const auto capped=planWith(2000);
    check(capped.targetUs==115023 && capped.capped==1,"a carry limit falls back to the physical ownership floor");
}

void extraDelayDoesNotChaseCurrentReadiness() {
    Vrr13::PhaseTrajectory p;
    p.plan(1,100000,100000,100000,100000,8333,0,500,10000,true,4000);
    p.commit(1,100000,104000,104000,true,false,true);
    uint64_t expectedExtra=0,previousMarginal=0;bool marginalChanged=false;
    for(uint64_t readyExtra:{0ULL,1000ULL,3000ULL,7000ULL}) {
        const auto s=p.plan(2,110000,110000,110000+readyExtra,110000+readyExtra,
                            8333,0,500,10000,true,4000);
        VrrTimingDecision d;d.originalTargetUs=110000;
        d.requestedPhaseTargetUs=s.requestedTargetUs;d.cadenceSmoothingUs=713;d.trajectory=s;
        const auto extra=d.plannedExtraDelayUs();
        if(!expectedExtra) expectedExtra=extra;
        check(extra==expectedExtra && extra==4588,
              "the same retained recovery phase reports the same extra despite alternating current readiness");
        check(s.targetUs==std::max(s.requestedTargetUs,110000+readyExtra),
              "extra accounting preserves the actual readiness-bounded target");
        marginalChanged|=previousMarginal && previousMarginal!=s.addedUs;
        previousMarginal=s.addedUs;
    }
    check(marginalChanged,"fixture reproduces the old marginal-wait reporting oscillation");
    Vrr13::PhaseTrajectory first;
    VrrTimingDecision d;d.originalTargetUs=100000;d.requestedPhaseTargetUs=100000;
    d.trajectory=first.plan(1,100000,100000,113000,113000,8333,0,500,10000,true,4000);
    check(d.plannedExtraDelayUs()==0 && d.trajectory.targetUs==113000,
          "a new late input stays a readiness overrun, not invented smoothing/recovery protection");
    d.requestedPhaseTargetUs=102000;
    check(d.plannedExtraDelayUs()==2000,"a requested physical floor remains visible independently of current readiness");
    d.cadenceSmoothingUs=-2500;
    check(d.plannedExtraDelayUs()==0,"negative smoothing cancels requested phase before the nonnegative clamp");
    d.trajectory={};d.originalTargetUs=0;d.requestedPhaseTargetUs=std::numeric_limits<uint64_t>::max();
    d.cadenceSmoothingUs=1;
    check(d.plannedExtraDelayUs()==std::numeric_limits<uint64_t>::max(),"extra arithmetic saturates without wrapping");
    d.requestedPhaseTargetUs=0;
    d.cadenceSmoothingUs=std::numeric_limits<int64_t>::min();
    check(d.plannedExtraDelayUs()==0,"the most negative phase clamps safely");
}

void decodeCompletionEstimate() {
    using C=VrrTimingController;
    check(C::estimateDecodeCompleteUs(1000,5000,3000,3800)==5000,"worker blocked on decode: its completion time is exact");
    check(C::estimateDecodeCompleteUs(1000,9000,20,3800)==4800,"finished while queued: output plus typical GPU decode, not the late observation");
    check(C::estimateDecodeCompleteUs(1000,3000,20,3800)==3000,"estimate never exceeds the observed completion bound");
    check(C::estimateDecodeCompleteUs(1000,9000,20,0)==9000,"without a learned GPU time the observation is used");
    check(C::estimateDecodeCompleteUs(1000,0,0,3800)==1000,"unknown completion falls back to decoder output");
}
void resyncAndLifecycle() {
    Vrr13::PhaseTrajectory p;p.commit(1,100000,106690,106690,true,false,true);
    auto s=p.plan(2,108333,108333,108333,108333,8333,115023,750,8333,true);
    check(s.noHeadroom && s.carryUs==6690 && !s.resyncRecommended,
        "same-refresh carry is shown late; recovery never recommends skipping a frame");
    p.noteDeferred();check(p.stats().deferred==1 && p.stats().resyncDrops==0,"unready successor records deferral without inventing a drop");
    p.noteResyncDrop();s=p.plan(3,116666,116666,116666,116666,8333,115023,750,8333,true);
    check(!s.continuity && s.targetUs>=115023 && p.stats().resyncDrops==1,
        "resync releases phase anchor while caller-owned physical floor remains enforced");
    p.reset();p.commit(1,100000,100100,100100,true,false,true);
    s=p.plan(2,108333,108333,108333,108333,8333,108433,750,8333,true);
    check(s.carryUs==100 && !s.resyncRecommended,"sub750us same-refresh carry does not trigger needless skip");
    p.reset();p.commit(1,100000,130000,130000,true,false,true);
    s=p.plan(2,109719,109719,109719,109719,8333,138333,750,9719,true);
    check(!s.resyncRecommended && s.targetUs==138333 && p.stats().capped==1,
        "carry beyond one source period is capped at the physical floor instead of skipping a frame");
    p.reset();p.commit(1,100000,130000,130000,true,false,true);
    check(!p.plan(2,109719,109719,109719,140000,8333,138333,750,9719,true).resyncRecommended,
        "ongoing input lateness is not represented as a clean-successor recovery opportunity");
    for(int failure=0;failure<3;++failure) {
        p.reset();p.commit(1,100000,100000,100000,true,false,true);
        p.commit(2,109719,110000,110000,failure!=0,failure==1,failure!=2);
        s=p.plan(3,119438,119438,119438,119438,8333,120000,750,9719,true);
        check(!s.continuity && s.targetUs>=120000,"failure cancellation and invalid acceptance break phase but preserve physical ownership floor");
    }
    p.reset();p.commit(1,100000,100000,100000,true,false,true);
    check(!p.plan(3,119438,119438,119438,119438,8333,120000,750,9719,true).continuity,"frame identity gap breaks trajectory evidence");
    p.reset();p.commit(1,100000,100000,100000,true,false,true);
    s=p.plan(2,119438,119438,119438,119438,8333,0,750,19438,false);
    check(!s.continuity,"source-rate or epoch invalidation breaks trajectory continuity");
    p.reset();check(p.stats().plans==0 && p.stats().deferred==0 && p.stats().resyncDrops==0,"full reset clears trajectory counters");
    const auto max=std::numeric_limits<uint64_t>::max();p.commit(1,max-20000,max-1000,max-1000,true,false,true);
    check(p.plan(2,max-10000,max-500,max-500,max-500,8333,max-1,750,10000,true).targetUs==max,"anchor and physical arithmetic saturate safely");
}
void preparedSuccessorProof() {
    VrrPreparedFrame pending;check(!pending.readyForPresentation(),"unfinished preparation is not a ready successor");
    pending.complete(true);check(pending.readyForPresentation(),"successful completed preparation proves ready successor");
    pending.cancel();check(!pending.readyForPresentation(),"cancelled completed preparation cannot replace active image");
    VrrPreparedFrame failed;failed.complete(false);check(!failed.readyForPresentation(),"failed preparation cannot replace active image");
    VrrPreparedFrame cancelled;cancelled.cancel();cancelled.complete(true);check(!cancelled.readyForPresentation(),"cancel-before-completion stays unready");
    VrrPreparedFrame concurrent;std::atomic_bool done{false};
    std::thread producer([&]{concurrent.timing.readyUs=1234;concurrent.complete(true);done=true;});
    while(!done.load()) concurrent.readyForPresentation();
    producer.join();check(concurrent.readyForPresentation() && concurrent.timing.readyUs==1234,"mutex readiness query publishes completed producer timing");
}
void sourceMappingFactorial() {
    VrrSessionConfig session;session.streamRateHz=60;session.displayRefreshHz=120;
    session.smoothFrameTiming=false;session.experimentalInputPercentilePpm=1;
    session.timingOptions.targetHundredths=9950;session.timingOptions.historySeconds=200;
    for(bool corrected:{false,true}) for(bool bounded:{false,true}) {
        auto policy=vrrTimingParametersForSession(session);policy.normalizedSourceMapping=corrected;policy.boundedCatchupEnabled=bounded;
        policy.playoutDelayMinimumUs=1000;policy.playoutDelayStartSeedUs=1000;policy.playoutDelayMaximumUs=6000;
        VrrTimingController reference(session,true,policy),queued(session,true,policy);
        uint64_t shifted=0,maxSourceDrift=0,eligible=0;
        for(int i=0;i<1800;++i) {
            const uint64_t source=1000000+uint64_t(i)*1000000/60;
            const uint64_t queue=i<300?0:10000+uint64_t(i-300)*5;
            const uint64_t hold=std::min<uint64_t>(queue,4000);
            uint64_t intended[2]={0,0};unsigned index=0;
            for(auto* c:{&reference,&queued}) {
                const uint64_t ownQueue=c==&queued?queue:0;
                PacedFrame f(nullptr,i,uint32_t(uint64_t(i)*1500),true,source+ownQueue);
                f.setDeliveryTimeline(source-800,source-500,source-500+ownQueue);
                f.setDecodeHoldUs(c==&queued?hold:0);
                auto d=c->schedule(f,source+ownQueue);
                intended[index++]=d.sourceTimeUs;
                const uint64_t ready=std::max(source+ownQueue,d.renderStartUs)+500;
                const uint64_t submission=std::max(ready,d.targetUs);
                c->notePreparationDuration(500,0,ready);c->noteSchedulerDelays(0,0,true);c->noteSubmission(true,false,submission,submission);
            }
            const uint64_t drift=intended[0]>intended[1]?intended[0]-intended[1]:intended[1]-intended[0];
            shifted+=drift>0;maxSourceDrift=std::max(maxSourceDrift,drift);
        }
        const auto stats=queued.intervalStats().inputPercentile;eligible=stats.eligible;
        if(corrected) check(maxSourceDrift==0 && eligible>1000 && stats.targetUs<=1000,
            "validated reassembly+active-service mapping removes prolonged own decoder queue and hold independently of trajectory switch");
        std::fprintf(stderr,"mapping factorial corrected=%d trajectory=%d shiftedFrames=%llu maxSourceDrift=%lluus eligible=%llu target=%lluus\n",corrected,bounded,(unsigned long long)shifted,(unsigned long long)maxSourceDrift,(unsigned long long)eligible,(unsigned long long)stats.targetUs);
    }
}
// PyroWave decodes asynchronously: decoder output is GPU submission and the
// frame is usable ~4 ms later. The normalized producer clock must plan from
// completion. Odd frames finish while queued, so the worker only observes them
// 5 ms late; their completion must come from the learned typical GPU time.
void asyncDecodeCompletionPlanning() {
    VrrSessionConfig session;session.streamRateHz=60;session.displayRefreshHz=120;
    session.smoothFrameTiming=false;session.experimentalInputPercentilePpm=1;session.experimentalBoundedCatchup=true;
    session.timingOptions.targetHundredths=9950;session.timingOptions.historySeconds=200;
    auto policy=vrrTimingParametersForSession(session);
    policy.playoutDelayMinimumUs=1000;policy.playoutDelayStartSeedUs=1000;policy.playoutDelayMaximumUs=6000;
    check(policy.normalizedSourceMapping!=0,"bounded-catchup sessions use the normalized producer clock");
    VrrTimingController c(session,true,policy);
    uint64_t planned=0,early=0,lead=0;
    for(int i=0;i<1200;++i) {
        const uint64_t source=1000000+uint64_t(i)*1000000/60;
        const uint64_t output=source+1000,complete=output+4000;
        const bool blocked=i%2==0;
        const uint64_t observed=blocked?complete:complete+5000;
        PacedFrame f(nullptr,i,uint32_t(uint64_t(i)*1500),true,output);
        f.setDeliveryTimeline(source-300,source,source);f.setDecoderOutputComplete(false);
        f.noteDecodeSyncWaitUs(blocked?4000:20);f.noteGpuReadyUs(observed);
        const auto d=c.schedule(f,observed);
        if(i>=300) {++planned;early+=d.originalTargetUs<complete;lead+=d.originalTargetUs-std::min(d.originalTargetUs,complete);}
        const uint64_t ready=std::max(observed,d.renderStartUs)+500;
        const uint64_t submission=std::max(ready,d.targetUs);
        c.notePreparationDuration(500,0,ready);c.noteSchedulerDelays(0,0,true);c.noteSubmission(true,false,submission,submission);
    }
    check(c.typicalDecodeGpuUs()==4000,"typical GPU decode is learned only from completions the worker blocked on");
    check(planned>0 && early==0,"no frame is planned before its actual GPU decode completion");
    std::fprintf(stderr,"async decode planning: early=%llu/%llu mean plan after completion=%lluus typicalGpu=%lluus\n",
        (unsigned long long)early,(unsigned long long)planned,(unsigned long long)(planned?lead/planned:0),(unsigned long long)c.typicalDecodeGpuUs());
}
// Bounded catch-up owns recovery. The VRR17 late-recovery floor re-armed on
// every submission later than the original plan, i.e. on every carried frame,
// and spaced each frame from the previous actual submission: at 116 FPS on a
// 120 Hz panel it may recover only 0.19 ms per frame, less than the usual
// 0.28 ms wake-up overshoot, so one late frame left carry that never drained
// (2026-10-10 23:38: carry p50 7.7 ms, 81% of frames pushed past plan).
void legacyRecoveryCannotSustainCarry() {
    VrrSessionConfig session;session.streamRateHz=116;session.displayRefreshHz=120;
    session.smoothFrameTiming=false;session.experimentalInputPercentilePpm=1;session.experimentalBoundedCatchup=true;
    session.timingOptions.targetHundredths=9950;session.timingOptions.historySeconds=200;
    auto policy=vrrTimingParametersForSession(session);
    policy.playoutDelayMinimumUs=1000;policy.playoutDelayStartSeedUs=1000;policy.playoutDelayMaximumUs=1000;
    VrrTimingController c(session,true,policy);
    const uint64_t period=1000000/116;
    std::vector<uint64_t> carry,pushed;
    uint64_t tailFrames=0,tailStillPushed=0;
    for(int i=0;i<2400;++i) {
        const uint64_t source=1000000+uint64_t(i)*period;
        const uint64_t decoded=source+500+(i%200==100?6000:0);
        PacedFrame f(nullptr,i,uint32_t(uint64_t(i)*90000/116),true,decoded);
        f.setDeliveryTimeline(decoded-800,decoded-600,decoded-500);
        const auto d=c.schedule(f,decoded);
        if(i>=1200) {carry.push_back(d.trajectory.carryUs);pushed.push_back(d.targetUs-d.originalTargetUs);}
        if(i>=1200 && i%200>=180) {
            ++tailFrames;
            tailStillPushed+=d.trajectory.carryUs!=0 || d.targetUs!=d.originalTargetUs;
        }
        const uint64_t ready=std::max(decoded,d.renderStartUs)+500;
        const uint64_t submission=std::max(ready,d.targetUs)+280;
        c.notePreparationDuration(500,0,ready);c.noteSchedulerDelays(0,0,true);c.noteSubmission(true,false,submission,submission);
    }
    std::sort(carry.begin(),carry.end());std::sort(pushed.begin(),pushed.end());
    const auto median=[](const std::vector<uint64_t>& v){return v[v.size()/2];};
    const auto p90=[](const std::vector<uint64_t>& v){return v[v.size()*9/10];};
    std::fprintf(stderr,"bounded catch-up: carry p50 %lluus p90 %lluus; target-plan p50 %lluus p90 %lluus\n",
        (unsigned long long)median(carry),(unsigned long long)p90(carry),(unsigned long long)median(pushed),(unsigned long long)p90(pushed));
    check(median(carry)==0 && median(pushed)==0,"between late frames the schedule returns to its plan");
    // A 6 ms spike exceeds the permitted buffer, so test actual convergence.
    check(tailFrames>0 && tailStillPushed==0,
        "late frames drain all carry before the next spike");
    check(p90(carry)<=policy.playoutCatchupCarryLimitUs,
        "catch-up retains the selected carry ceiling");
}
void actualCadenceChangeQualification() {
    VrrSessionConfig session;session.streamRateHz=120;session.displayRefreshHz=144;session.smoothFrameTiming=false;
    session.experimentalInputPercentilePpm=1;session.experimentalBoundedCatchup=true;
    auto policy=vrrTimingParametersForSession(session);VrrTimingController controller(session,true,policy);
    uint64_t at=1000000;uint32_t rtp=0;unsigned changes=0;bool previousChanged=false;
    for(int i=0;i<1200;++i) {
        if(i) {at+=i<300?16667:8333;rtp+=i<300?1500:750;}
        PacedFrame f(nullptr,i,rtp,true,at);f.setDeliveryTimeline(at-800,at-600,at-500);
        const auto d=controller.schedule(f,at);
        if(previousChanged) check(!d.trajectory.continuity,"source-rate transition cannot commit an unqualified new trajectory anchor");
        changes+=d.sourceRateChanged;previousChanged=d.sourceRateChanged;
        const auto ready=std::max(at,d.renderStartUs)+500;
        controller.notePreparationDuration(500,0,ready);controller.noteSchedulerDelays(0,0,true);
        controller.noteSubmission(true,false,std::max(ready,d.targetUs),std::max(ready,d.targetUs));
    }
    check(changes>0,"actual cadence fixture detects60to120Hz source-rate change");
    std::fprintf(stderr,"trajectory cadence60to120Hz sourceRateChanges=%u\n",changes);
}
void actualControllerAndInputIsolation() {
    VrrSessionConfig session;session.streamRateHz=60;session.displayRefreshHz=120;
    session.smoothFrameTiming=false;session.experimentalInputPercentilePpm=1;
    session.timingOptions.targetHundredths=9950;session.timingOptions.historySeconds=200;
    check(!vrrTimingParametersForSession(session).boundedCatchupEnabled,"bounded trajectory is disabled by default independently of adaptive mode");
    auto boundedSession=session;boundedSession.experimentalBoundedCatchup=true;
    auto policy=vrrTimingParametersForSession(boundedSession);
    check(policy.boundedCatchupEnabled && policy.playoutInputPercentileEnabled,"explicit bounded trajectory activation composes with selected adaptive profile");
    policy.playoutDelayMinimumUs=1000;policy.playoutDelayStartSeedUs=1000;policy.playoutDelayMaximumUs=6000;
    VrrTimingController originA(boundedSession,true,policy),originB(boundedSession,true,policy);
    PacedFrame originFrameA(nullptr,0,0,true,1000000),originFrameB(nullptr,0,0,true,1005000);
    const auto originDecisionA=originA.schedule(originFrameA,1000000);
    const auto originDecisionB=originB.schedule(originFrameB,1005000);
    check(originDecisionB.sourceTimeUs-originDecisionA.sourceTimeUs==5000,
        "identical RTP identity with shifted decoder arrival shifts source epoch origin: bounded catchup does not make source clock exogenous");
    VrrTimingController clean(boundedSession,true,policy),late(boundedSession,true,policy);
    uint64_t limited=0,peak=0;bool changedSlot=false;
    for(int i=0;i<1800;++i) {
        const auto decoded=1000000+uint64_t(i)*1000000/60;
        uint64_t sourceSlots[2]={0,0};unsigned which=0;
        for(auto* controller:{&clean,&late}) {
            PacedFrame frame(nullptr,i,uint32_t(uint64_t(i)*1500),true,decoded);
            frame.setDeliveryTimeline(decoded-800,decoded-600,decoded-500);
            auto d=controller->schedule(frame,decoded);
            sourceSlots[which++]=d.sourceTimeUs;
            if(controller==&late) {limited+=d.trajectory.addedUs>0;peak=std::max(peak,d.trajectory.carryUs);}
            if(d.trajectory.continuity) check(d.trajectory.minimumIntervalUs>=d.trajectory.physicalPeriodUs &&
                d.trajectory.minimumIntervalUs>=d.trajectory.intendedIntervalUs-std::min(d.trajectory.intendedIntervalUs,d.trajectory.allowanceUs),
                "actual controller uses separate intended-slot and physical floors");
            const auto ready=std::max(decoded,d.renderStartUs)+500;
            auto submitted=std::max(ready,d.targetUs);
            if(controller==&late && i==500) submitted+=6690;
            controller->notePreparationDuration(500,0,ready);controller->noteSchedulerDelays(0,0,true);
            controller->noteSubmission(true,false,submitted,submitted);
        }
        changedSlot|=sourceSlots[0]!=sourceSlots[1];
    }
    const auto a=clean.intervalStats().inputPercentile,b=late.intervalStats().inputPercentile;
    check(limited>0 && peak>0,"actual bounded controller records post-stall trajectory protection");
    check(a.eligible>1000 && b.eligible>1000 && b.currentUs<=1000 && b.targetUs<=1000,
        "late output ownership does not inflate causal input buffer for ready producer input");
    std::fprintf(stderr,"trajectory controller: limited=%llu peakCarry=%lluus sourceMappingChanged=%d (source slot remains endogenous; no independent-clock fix claimed)\n",(unsigned long long)limited,(unsigned long long)peak,changedSlot);
    late.rebase();check(!late.trajectoryStats().continuity || late.trajectoryStats().resets>0,"controller rebase invalidates accepted phase anchor");
    late.reset();check(late.trajectoryStats().plans==0,"controller full reset clears trajectory model");
}
}
int main() {extraDelayDoesNotChaseCurrentReadiness();bufferReleaseDoesNotCreateCarry();carryRepaymentKnobs();decodeCompletionEstimate();legacyRecoveryCannotSustainCarry();asyncDecodeCompletionPlanning();compressionAndAcceptance();resyncAndLifecycle();preparedSuccessorProof();sourceMappingFactorial();actualCadenceChangeQualification();actualControllerAndInputIsolation();std::fprintf(stderr,"phase trajectory integration: %s (%d failures)\n",failures?"FAIL":"PASS",failures);return failures?1:0;}
