// Standalone header-only lifecycle/clock contract integration tests.
#include "../../app/streaming/video/ffmpeg-renderers/pacer/vrr/inputbuffer.h"
#include "../../app/streaming/video/ffmpeg-renderers/pacer/vrr/inputlayercadence.h"
#include "../../app/streaming/video/ffmpeg-renderers/pacer/vrr/inputcoveragehistory.h"
#include "../../app/streaming/video/ffmpeg-renderers/pacer/vrr/extrabufferslew.h"
#include "../../app/streaming/video/performancestats.h"
#ifdef INPUT_PERCENTILE_CONTROLLER_TEST
#include "../../app/streaming/video/ffmpeg-renderers/pacer/vrr/vrrtimingcontroller.h"
#endif
#include <cstdio>
#include <cstdlib>
#include <limits>
#include <utility>
using Vrr13::InputGoalBuffer;
namespace {
int failures=0;
void check(bool ok,const char* what) {
    if(!ok) { ++failures;std::fprintf(stderr,"FAIL: %s\n",what); }
}
void testFastLayerCadence() {
    Vrr13::InputLayerCadence detector;
    check(Vrr13::InputLayerCadence::sameLayer(11111,9091) &&
          Vrr13::InputLayerCadence::sameLayer(8621,11111),
          "90-116 FPS variation shares one symmetric rate layer");
    check(!detector.observe(33333,11111,true) && !detector.observe(33334,11111,true) &&
          detector.observe(33333,11111,true)==33333,
          "90 to 30 FPS qualifies in three adjacent source intervals");
    check(!detector.observe(11111,33333,true) && !detector.observe(11112,33333,true) &&
          detector.observe(11111,33333,true)==11111,
          "return to 90 FPS qualifies in three intervals without waiting for the fit");
    detector.observe(33333,11111,true);
    check(!detector.observe(11111,11111,true) && !detector.observe(33333,11111,true),
          "an isolated long source interval does not switch layers");
    detector.observe(33333,11111,false);
    check(!detector.observe(33333,11111,true) && !detector.observe(33333,11111,true),
          "missing frame identities break fast rate qualification");
    detector.reset();
    for(int i=0;i<20;++i)
        check(!detector.observe(i%2?33333:50000,11111,true),
              "inconsistent slow intervals do not manufacture a stable new cadence");
}

void testConfirmedLayerCadence() {
    Vrr13::InputLayerCadence detector;
    for (const auto interval : {12645,13311,13600})
        check(!detector.observeConfirmed(interval,8621,11095,true),
              "reported 12-13 ms tail cannot switch away from a stale 116 FPS anchor");
    detector.reset();
    for (int i=0;i<3;++i)
        check(detector.observeConfirmed(33333,8621,11111,true)==(i==2?33333u:0u),
              "a real 90 to 30 FPS step qualifies after three source intervals");
    for (int i=0;i<3;++i)
        check(detector.observeConfirmed(10000,33333,33333,true)==(i==2?10000u:0u),
              "a real 30 to 100 FPS return still qualifies quickly");
    detector.reset();
    detector.observeConfirmed(33333,11111,11111,true);
    detector.observeConfirmed(33333,11111,11111,false);
    check(!detector.observeConfirmed(33333,11111,11111,true) &&
          !detector.observeConfirmed(33333,11111,11111,true),
          "missing identity interrupts qualification of a real rate change");
    detector.reset();
    for (int i=0;i<11;++i)
        check(!detector.observeConfirmed(33333,11111,33333,true),
              "a gradual departure needs sustained settled evidence");
    check(detector.observeConfirmed(33333,11111,33333,true)==33333,
          "a settled gradually reached 30 FPS rate eventually earns its own layer");
    detector.reset();
    for (int i=0;i<200;++i)
        check(!detector.observeConfirmed(i%2?13000:16000,8621,14000,true),
              "variable output beyond an old anchor is not a settled separate cadence");
    detector.reset();
    for(int i=0;i<100;++i)
        check(!detector.observeConfirmed(50000,25000,25000,true,30),
              "40 to 20 FPS cannot switch layers despite a twofold period change");
    detector.reset();
    for(int i=0;i<3;++i)
        check(detector.observeConfirmed(33333,11111,11111,true,30)==(i==2?33333u:0u),
              "90 to 30 FPS still qualifies in three frames with wide layers");
    check(Vrr13::InputLayerCadence::sameOperatingRate(25000,50000,30) &&
          !Vrr13::InputLayerCadence::sameOperatingRate(16667,33333,30),
          "lookup also enforces the 30 FPS separation and tolerates RTP rounding");
}

void testContinuousCoverage() {
    Vrr13::InputCoverageHistory history;
    InputGoalBuffer::Stats stats;
    history.observe(1000000,10000000,true,false,true);
    history.observe(2000000,10000000,true,true,false);
    history.observe(3000000,10000000,false,false,false);
    history.observe(1500000,10000000,true,false,false);
    // An empty new layer and a rebase are reporting events, not erased outcomes.
    history.report(stats);
    check(stats.recentEligible==2 && stats.recentMissed==1 && stats.recentAbsorbed==1,
          "coverage retains actual outcomes and ignores invalid or stale callbacks");
    PerformanceStats::Snapshot overlay;
    overlay.vrr=overlay.adaptive=true;overlay.targetPpm=995000;overlay.input=stats;
    check(PerformanceStats::formatVrr(overlay).find("Input coverage: 50.00%")!=std::string::npos,
          "input coverage survives an empty layer without inventing successes or a coverage target");
    history.advance(11000000,10000000);history.report(stats);
    check(stats.recentEligible==1 && stats.recentMissed==0 && stats.recentAbsorbed==1,
          "outcomes expire individually at the selected trailing window");
    history.advance(12000000,10000000);history.report(stats);
    check(stats.recentEligible==0,"coverage becomes unavailable when all real observations expire");
}

void testResponsiveCoverageReporting() {
    Vrr13::InputCoverageHistory history;
    InputGoalBuffer::Stats stats;
    history.observe(1000000,120000000,true,false,true);
    history.observe(9000000,120000000,true,true,false);
    history.observe(11000000,120000000,true,false,false);
    history.report(stats,true);
    check(stats.recentEligible==3 && stats.recentMissed==1 &&
          stats.reportingWindowUs==10000000 && stats.reportingEligible==2 &&
          stats.reportingMissed==0 && stats.reportingAbsorbed==1,
          "the recent score recovers after ten seconds while the selected history retains its miss");
    history.observe(12000000,120000000,true,false,true);
    history.observe(12000001,120000000,false,false,false);
    history.observe(5000000,120000000,true,false,false);
    history.report(stats,true);
    check(stats.reportingEligible==3 && stats.reportingMissed==1 && stats.recentMissed==2,
          "fresh misses update both scores immediately and invalid or stale callbacks never add successes");
    history.advance(22000000,120000000);history.report(stats,true);
    check(stats.reportingEligible==0 && stats.reportingMissed==0 && stats.recentMissed==2,
          "idle recent feedback becomes unavailable rather than inventing clean frames or erasing long history");
    history={};history.report(stats,true);
    check(stats.recentEligible==0 && stats.reportingEligible==0,
          "a new session clears both reporting windows");
}
void testSharedExtraBufferAllowance() {
    for(const uint64_t tolerance : {250,500,2000}) {
        Vrr13::ExtraBufferSlew slew;
        uint64_t input=1000;
        int64_t smoothing=2*tolerance;
        uint64_t recovery=0;
        slew.begin(0,1000000,input,tolerance,smoothing,false,false);
        for(uint64_t f=1;f<=500;++f) {
            const auto previous=input+uint64_t(smoothing)+recovery;
            if(f<=100) input=1000+f*tolerance/100;
            slew.begin(f,1000000+f*10000,input,tolerance,2*tolerance,false,false);
            smoothing=slew.smoothing(10000);
            recovery=slew.recovery(10000);
            const auto total=input+uint64_t(smoothing)+recovery;
            check(total>=previous && total-previous<=(tolerance+99)/100,
                  "input, smoothing and recovery share one T-per-second movement allowance");
        }
        check(uint64_t(smoothing)+recovery+input<=1000+2*tolerance+5*tolerance,
              "multiple extra-buffer paths cannot each spend the full five-second allowance");
        const auto before=smoothing;
        slew.begin(501,6010000,input,tolerance,2*tolerance,true,true);
        smoothing=slew.smoothing(0);
        slew.begin(501,6020000,input,tolerance,2*tolerance,false,true);
        smoothing=slew.smoothing(0);
        check(before-smoothing<=int64_t(tolerance),
              "repeated planning of one layer-transition frame cannot drain T twice");
    }
}

#ifdef INPUT_PERCENTILE_CONTROLLER_TEST
void testFactoryProfileOwnsAdaptiveTarget() {
    for (int mode : {0, 1, 2}) {
        for (int custom : {0, 9900, 9950, 9999, -1, 12, 10000, 999999}) {
            VrrSessionConfig session;
            session.streamRateHz = 120; session.displayRefreshHz = 144;
            session.latencyMode = mode; session.timingOptions.targetHundredths = custom;
            session.timingOptions.historySeconds = custom == 0 ? 0 : 200;
            const auto options = session.timingOptions.resolved(mode);
            session = vrrProductionSessionConfig(session);
            const auto policy = vrrTimingParametersForSession(session);
            check(policy.playoutInputPercentileEnabled &&
                  policy.playoutInputPercentileTargetPerMillion == uint64_t(options.targetHundredths) * 100 &&
                  policy.playoutInputPercentileToleranceUs == uint64_t(options.toleranceUs) &&
                  policy.playoutInputPercentileWindowUs == uint64_t(options.historySeconds) * 1000000 &&
                  policy.playoutOnTimeTargetPerMillion == uint64_t(options.targetHundredths) * 100,
                  "production resolves custom target, tolerance and history together");
        }
    }
}
void testActualControllerRateBandsRestoreProtection() {
    // The user's round trip: 120 FPS needs buffer, 30 FPS needs none (and has
    // less lateness, as in the 22:55 capture), then back to 120 FPS.
    VrrSessionConfig session;session.streamRateHz=120;session.displayRefreshHz=144;
    session.smoothFrameTiming=false;
    session.timingOptions={2000,9950,60,500};
    session = vrrProductionSessionConfig(session);
    auto policy=vrrTimingParametersForSession(session);
    policy.playoutDelayStartSeedUs=1000;
    VrrTimingController controller(session,true,policy);
    uint64_t submitted=0,slot=1000000,pts=0;int i=0;
    const auto run=[&](int frames,uint64_t periodUs,uint32_t ptsStep,uint64_t spikeUs) {
        for(int n=0;n<frames;++n,++i) {
            slot+=periodUs;pts+=ptsStep;
            const uint64_t decoded=slot+1000+(i%20==0?spikeUs:0);
            PacedFrame f(nullptr,i,uint32_t(pts),true,decoded);
            f.setDeliveryTimeline(decoded-800,decoded-600,decoded-500);
            const auto decision=controller.schedule(f,std::max(decoded,submitted));
            const uint64_t ready=std::max(decoded,decision.renderStartUs)+500;
            submitted=std::max(ready,decision.targetUs);
            controller.notePreparationDuration(500,0,ready);
            controller.noteSchedulerDelays(0,0,true);
            controller.noteSubmission(true,false,submitted,submitted);
        }
    };
    run(2400,8333,750,5000);
    const auto fast=controller.intervalStats().inputPercentile;
    run(600,33333,3000,1000);
    const auto slow=controller.intervalStats().inputPercentile;
    run(36,8333,750,5000);
    const auto back=controller.intervalStats().inputPercentile;
    run(564,8333,750,5000);
    const auto settled=controller.intervalStats().inputPercentile;
    const uint64_t missedBack=settled.missed-slow.missed,eligibleBack=settled.eligible-slow.eligible;
    std::fprintf(stderr,"rate bands: 120 FPS current %llu target %llu; 30 FPS current %llu; back +36 frames current %llu; "
        "missed after return %llu of %llu; coverage %llu/%llu\n",
        (unsigned long long)fast.currentUs,(unsigned long long)fast.targetUs,(unsigned long long)slow.currentUs,
        (unsigned long long)back.currentUs,(unsigned long long)missedBack,(unsigned long long)eligibleBack,
        (unsigned long long)(settled.recentEligible-settled.recentMissed),(unsigned long long)settled.recentEligible);
    check(settled.eligible>=slow.eligible && slow.eligible>=fast.eligible && settled.missed>=slow.missed,
          "session totals continue across rate-band layers");
    check(fast.currentUs>2000,"fixture: 120 FPS jitter needs real buffer");
    check(slow.currentUs<fast.currentUs,"30 FPS headroom releases protection");
    check(back.currentUs+250>=fast.currentUs,"returning to 120 FPS restores that band's protection within a few frames");
}

void testActualControllerRepeatedZeroTimestampDrops() {
    VrrSessionConfig session;session.streamRateHz=60;session.displayRefreshHz=120;
    session.smoothFrameTiming=false;session.experimentalInputPercentilePpm=990000;
    session.timingOptions.historySeconds=30;
    session = vrrProductionSessionConfig(session);
    auto policy=vrrTimingParametersForSession(session);
    policy.playoutDelayStartSeedUs=6000;
    VrrTimingController controller(session,true,policy);
    uint64_t initial=0;
    for(int i=0;i<6000;++i) {
        if(i && i%60==0) controller.rebase();
        const uint64_t now=1000000+uint64_t(i)*1000000/60;
        PacedFrame frame(nullptr,i,uint32_t(uint64_t(i)*1500),true,now);
        // No preparation proof and no accepted/CPU submission instant.
        const auto decision=controller.schedule(frame,now);
        if(!i) initial=decision.playoutDelayUs;
        controller.noteSubmission(false,false,0);
    }
    const auto stats=controller.intervalStats().inputPercentile;
    check(initial>stats.minimumUs+2000,"zero-timestamp drop fixture starts with meaningful seeded protection");
    check(stats.observed>5000 && stats.eligible==0 && stats.ineligible==stats.observed,
        "actual controller observes dropped frames on scheduling clock without treating them as valid input");
    check(stats.expiredUnknown && stats.currentUs==stats.minimumUs && stats.epochResets>50,
        "repeated zero-timestamp drops and phase resets release obsolete seed after history expiry and fall slew");
}
void testActualControllerPostInputErrorsCannotInflateBuffer() {
    VrrSessionConfig session;session.streamRateHz=60;session.displayRefreshHz=120;
    session.latencyMode=1;session.smoothFrameTiming=false;session.experimentalInputPercentilePpm=1;
    session.timingOptions={1000,9950,10,500};
    session = vrrProductionSessionConfig(session);
    auto policy=vrrTimingParametersForSession(session);
    policy.playoutDelayStartSeedUs=1000;policy.playoutDelayMinimumUs=1000;
    policy.playoutDelayMaximumUs=6000;
    VrrTimingController clean(session,true,policy),outputErrors(session,true,policy);
    uint64_t peak=0;
    for(int i=0;i<2400;++i) {
        const uint64_t decoded=1000000+uint64_t(i)*1000000/60;
        for(auto* controller:{&clean,&outputErrors}) {
            if(i && i%600==0) controller->rebase();
            PacedFrame frame(nullptr,i,uint32_t(uint64_t(i)*1500),true,decoded);
            frame.setDeliveryTimeline(decoded-800,decoded-600,decoded-500);
            const auto decision=controller->schedule(frame,decoded);
            const uint64_t ready=std::max(decoded,decision.renderStartUs)+500;
            uint64_t submission=std::max(ready,decision.targetUs);
            if(controller==&outputErrors && i>120) submission+=i%2?4000:0;
            controller->notePreparationDuration(500,0,ready);
            controller->noteSchedulerDelays(0,0,true);
            controller->noteSubmission(true,false,submission,submission);
        }
        const auto baseline=clean.intervalStats().inputPercentile;
        const auto errored=outputErrors.intervalStats().inputPercentile;
        peak=std::max(peak,errored.currentUs);
        check(errored.currentUs<=baseline.currentUs && errored.targetUs<=baseline.targetUs,
            "post-input compositor/submission interval errors cannot inflate direct causal input headroom");
    }
    const auto a=clean.intervalStats(),b=outputErrors.intervalStats();
    check(b.inputPercentile.eligible>1000 && peak==1000 && b.qualityPercent()<a.qualityPercent(),
        "integrated fixture reports repeated output timing failures while input remains within unchanged headroom");
    check(b.inputPercentile.lastInputRequiredUs==a.inputPercentile.lastInputRequiredUs,
        "own buffering and rebases preserve causal input requirement under downstream errors");
}
void testActualControllerEligibility() {
    VrrSessionConfig session;
    session.streamRateHz = 120; session.displayRefreshHz = 144;
    session.smoothFrameTiming = false;
    session = vrrProductionSessionConfig(session);
    const auto policy = vrrTimingParametersForSession(session);
    VrrTimingController controller(session,true,policy);
    uint64_t submitted=0;
    for(int i=0;i<1500;++i) {
        const uint64_t decoded=1000000+uint64_t(i)*1000000/120;
        PacedFrame f(nullptr,i,uint32_t(uint64_t(i)*750),true,decoded);
        f.setDeliveryTimeline(decoded-800,decoded-600,decoded-500);
        const auto decision=controller.schedule(f,std::max(decoded,submitted));
        const uint64_t ready=std::max(decoded,decision.renderStartUs)+500;
        submitted=std::max(ready,decision.targetUs);
        controller.notePreparationDuration(500,0,ready);
        controller.noteSchedulerDelays(0,0,true);
        controller.noteSubmission(true,false,submitted,submitted);
    }
    auto stats=controller.intervalStats();
    check(stats.inputPercentile.toleranceUs==policy.playoutInputPercentileToleranceUs,
        "actual controller feeds selected input tolerance without changing causal eligibility qualification");
    check(stats.inputPercentileEnabled && stats.inputPercentile.observed>1000 && stats.inputPercentile.eligible>1000,
        "actual controller feeds causal timeline input after qualified service calibration");
    check(stats.update.requestedUs==stats.inputPercentile.currentUs &&
        stats.update.minimumUs==stats.inputPercentile.minimumUs && stats.update.maximumUs==stats.inputPercentile.maximumUs,
        "published interval request follows direct percentile demand and dynamic bounds");
    const auto eligible=stats.inputPercentile.eligible;
    const auto samples=stats.inputPercentile.samples;
    const uint64_t decoded=1000000+1500ULL*1000000/120;
    PacedFrame lost(nullptr,1500,1500*750,true,decoded);
    lost.setDeliveryTimeline(decoded-800,decoded-600,decoded-500);lost.setLostPackets(1);
    const auto decision=controller.schedule(lost,std::max(decoded,submitted));
    const uint64_t ready=std::max(decoded,decision.renderStartUs)+500;
    submitted=std::max(ready,decision.targetUs);
    controller.notePreparationDuration(500,0,ready);controller.noteSchedulerDelays(0,0,true);
    controller.noteSubmission(true,false,submitted,submitted);
    stats=controller.intervalStats();
    check(stats.inputPercentile.eligible==eligible && stats.inputPercentile.samples==samples,
        "lost packets exclude percentile learning in actual production controller");
    const auto epoch=stats.inputPercentile.epochResets;
    const auto current=stats.inputPercentile.currentUs;
    controller.rebase();stats=controller.intervalStats();
    check(stats.inputPercentile.samples==0 && stats.inputPercentile.currentUs==current && stats.inputPercentile.epochResets>epoch,
        "controller source epoch clears percentile basis while preserving protection");
    controller.reset();check(!controller.intervalStats().inputPercentile.initialized,
        "full controller reset clears direct control state for next activation");
}
#endif

} // namespace

static void testGoalSolverMeetsGoalWithLeastBuffer() {
    // 1000 pairs at 8333 us; every 100th frame arrives 6 ms late.
    Vrr13::InputGoalSolver solver; int64_t previous=0;
    for(int i=1;i<=1000;++i){const int64_t late=i%100==0?6000:0;solver.add(previous,late,8333,500);previous=late;}
    check(solver.jitterPairs()==19,"goal solver keeps only pairs whose lateness differs beyond tolerance");
    const auto met=solver.solve(995000,500,0,8620,250);
    check(met.goalReachable && solver.loss(met.bufferUs,500)<=0.005 &&
          (met.bufferUs==0 || solver.loss(met.bufferUs-250,500)>0.005),
          "goal solver returns the smallest grid buffer that meets the goal");
    check(met.bufferUs<6000,"a 99.5% goal tolerates part of a 2% late tail without covering all of it");
    const auto strict=solver.solve(999500,500,0,8620,250);
    check(strict.goalReachable && strict.bufferUs>=5500 && strict.bufferUs<=6000,
          "a 99.95% goal covers the late frames to within the tolerance");
}
static void testGoalSolverUnreachableGoals() {
    // Half the frames arrive 12 ms late: an 8.6 ms maximum reduces but cannot remove the loss.
    Vrr13::InputGoalSolver solver; int64_t previous=0;
    for(int i=1;i<=1000;++i){const int64_t late=i%2?12000:0;solver.add(previous,late,8333,500);previous=late;}
    const auto solution=solver.solve(995000,500,0,8620,250);
    check(!solution.goalReachable && solution.bufferUs==8620,
          "an unreachable goal protects as much as the cap permits");
    Vrr13::InputGoalSolver flat; flat.add(12000,24000,8333,500);
    const auto plateau=flat.solve(995000,500,1000,8620,250);
    check(!plateau.goalReachable && plateau.bufferUs==1000,
          "equal timing scores prefer the smallest buffer");
}
static void testGoalBufferFollowsRecentJitter() {
    Vrr13::InputGoalBuffer buffer; buffer.setGoalSolver(20000000,1000);
    uint64_t t=1000000,frame=0,slot=1000000;
    auto feed=[&](int seconds,int spikeEvery){for(int i=0;i<seconds*120;++i){t+=8333;slot+=8333;++frame;
        const int64_t late=spikeEvery&&frame%spikeEvery==0?5000:0;
        buffer.advance(t,0,8620,995000,60000000,4000,1000,0,500,0,0);
        buffer.observe(frame,t,slot,late,true,0,8620,995000,60000000,4000,1000,0,500,0,0,late>500);}};
    feed(30,50);
    const uint64_t grown=buffer.stats().currentUs;
    check(grown>=3500 && grown<=5000,"recurring 5 ms spikes above the goal's tail grow the buffer to cover them");
    feed(60,0);
    check(buffer.stats().currentUs==0,"clean input releases the buffer once the spikes leave the solver window");
}

static void testGoalBufferLifecycle() {
    Vrr13::InputGoalBuffer buffer;

    buffer.setGoalSolver( 10000000, 700);
    uint64_t at = 1000000, slot = 1000000;
    for (uint64_t frame = 1; frame <= 480; ++frame) {
        at += 8333; slot += 8333;
        const int64_t late = frame % 10 == 0 ? 5000 : 0;
        buffer.advance(at, 1000, 8620, 995000, 10000000, 4000, 1000, 0, 500, 0, 0);
        buffer.observe(frame, at, slot, late, true, 1000, 8620, 995000,
                       10000000, 4000, 1000, 0, 500, 0, 0, late > 500);
    }
    auto stats = buffer.stats();
    check(stats.samples == 480 && stats.eligible == 480 && stats.currentUs > 1000,
          "the current goal solver learns and raises protection from sustained input jitter");
    const auto before = stats.currentUs;
    buffer.breakAdjacency();
    stats = buffer.stats();
    check(stats.samples == 480 && stats.currentUs == before,
          "a cadence break retains learned input protection without inventing a pair");
    buffer.markEvidenceAsSeed();
    stats = buffer.stats();
    check(stats.samples == 480 && stats.recentEligible == 0 && stats.currentUs == before,
          "a seeded rate layer retains evidence but does not copy coverage outcomes");
    buffer.noteBufferMiss(at, true, false);
    check(buffer.stats().cumulativeNetworkMisses == 1,
          "a network-stage miss remains diagnostic");
    buffer.breakSequence();
    stats = buffer.stats();
    check(stats.samples == 0 && stats.currentUs == before && stats.epochResets == 1,
          "a source rebase clears evidence and retains applied protection");
    buffer.advance(at + 11000000, 1000, 8620, 995000, 10000000,
                   4000, 1000, 0, 500, 0, 0);
    stats = buffer.stats();
    check(stats.expiredUnknown && stats.targetUs == 1000,
          "expired evidence releases toward the minimum without inventing observations");
}

#ifdef INPUT_PERCENTILE_CONTROLLER_TEST
static void testProductionUsesGoalSolver() {
    struct Expect { int mode; uint64_t windowUs, capPerMille; };
    for(const auto e:{Expect{2,20000000,500},Expect{1,60000000,1000},Expect{0,120000000,2000}}){
        VrrSessionConfig session;session.streamRateHz=116;session.displayRefreshHz=120;session.latencyMode=e.mode;
        session.experimentalInputPercentilePpm=1;session.experimentalBoundedCatchup=true;
        session.timingOptions=VrrTimingOptions::preset(e.mode);
        const auto policy=vrrTimingParametersForSession(session);
        check(policy.playoutInputGoalWindowUs==e.windowUs &&
              policy.playoutInputGoalTailPerMille==700 && policy.playoutInputIntrinsicDecodeGpu==1 &&
              policy.playoutInputPercentileRiseUsPerSecond==4000 && policy.playoutInputPercentileFallUsPerSecond==1000 &&
              policy.playoutDelayMaximumPeriodPerMille==e.capPerMille,
              "each production profile disables the latency price and keeps its buffer cap");
    }
}
#endif

int main() {
    testFastLayerCadence();testConfirmedLayerCadence();testContinuousCoverage();testResponsiveCoverageReporting();
    testSharedExtraBufferAllowance();
    testGoalSolverMeetsGoalWithLeastBuffer();testGoalSolverUnreachableGoals();testGoalBufferFollowsRecentJitter();testGoalBufferLifecycle();
#ifdef INPUT_PERCENTILE_CONTROLLER_TEST
    testProductionUsesGoalSolver();
    testFactoryProfileOwnsAdaptiveTarget();
#endif
#ifdef INPUT_PERCENTILE_CONTROLLER_TEST
    testActualControllerEligibility();
    testActualControllerRateBandsRestoreProtection();
    testActualControllerRepeatedZeroTimestampDrops();
    testActualControllerPostInputErrorsCannotInflateBuffer();
#endif
    std::fprintf(stderr,"input goal-buffer lifecycle integration tests: %s (%d failures)\n",failures?"FAIL":"PASS",failures);
    return failures?1:0;
}
