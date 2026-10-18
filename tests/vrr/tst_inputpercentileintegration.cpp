// Standalone header-only lifecycle/clock contract integration tests.
#include "../../app/streaming/video/ffmpeg-renderers/pacer/vrr/inputpercentilebuffer.h"
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
using Vrr13::InputPercentileBuffer;
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
    InputPercentileBuffer::Stats stats;
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
    check(PerformanceStats::formatVrr(overlay).find("coverage 50.00% / 99.50% target")!=std::string::npos,
          "the displayed timing percentage survives an empty layer without invented successes");
    history.advance(11000000,10000000);history.report(stats);
    check(stats.recentEligible==1 && stats.recentMissed==0 && stats.recentAbsorbed==1,
          "outcomes expire individually at the selected trailing window");
    history.advance(12000000,10000000);history.report(stats);
    check(stats.recentEligible==0,"coverage becomes unavailable when all real observations expire");
}

void testResponsiveCoverageReporting() {
    Vrr13::InputCoverageHistory history;
    InputPercentileBuffer::Stats stats;
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
void testGradualLayerGrowthAndHealthyRelease() {
    InputPercentileBuffer current, historical, delayed, noRoom;
    current.reset(6000); historical.reset(6000); noRoom.reset(6000);
    uint64_t previous = 6000;
    for(uint64_t f=1;f<=200;++f) {
        for(auto pair : {std::pair<InputPercentileBuffer*,uint64_t>{&current,7}, {&historical,6}})
            pair.first->observe(f,1000000+f*10000,2000000+f*10000,9000,true,
                1000,12000,995000,120000000,1000,250,0,500,1000,0,false,pair.second);
        noRoom.observe(f,1000000+f*10000,2000000+f*10000,9000,true,
            1000,12000,995000,120000000,1000,250,0,500,0,0,false,7);
        const auto s=current.stats();
        check(s.currentUs<=previous && previous-s.currentUs<=50,
              "healthy release is monotonic and bounded to 0.5 percent per interval");
        previous=s.currentUs;
        if(f==100) delayed=current;
    }
    const uint64_t beforeDelayedDecision=delayed.stats().currentUs;
    delayed.advance(2050000);
    delayed.advance(2060000);
    check(beforeDelayedDecision-delayed.stats().currentUs<=50,
          "late or repeated decisions cannot spend more than one source interval's release allowance");
    check(current.stats().currentUs<2000 && current.stats().missed==0 &&
          historical.stats().currentUs==6000,
          "healthy coverage drains a raw-percentile plateau while recorded policy retains it");
    check(noRoom.stats().currentUs==6000 && noRoom.stats().effectiveFallRateUsPerSecond==250,
          "healthy coverage without VRR room cannot authorize a release probe");
    for(uint64_t f=201;f<=204;++f) {
        const auto before=current.stats().currentUs;
        current.observe(f,1000000+f*10000,2000000+f*10000,8000,true,
            1000,12000,995000,120000000,1000,250,0,500,1000,0,true,7);
        const auto s=current.stats();
        check(s.currentUs<=before+10,"ordinary growth respects 1000 us/s instead of jumping");
    }
    check(current.stats().targetUs>current.stats().currentUs &&
          current.stats().effectiveFallRateUsPerSecond==250,
          "new goal-breaking input misses stop accelerated release and retain growth demand");
}
void testFeedbackBoundedRelease() {
    const auto observe=[](InputPercentileBuffer& p,uint64_t f,int64_t required,
                          bool hitch=false,bool valid=true,uint64_t goal=995000) {
        p.observe(f,1000000+f*10000,2000000+f*10000,required,valid,
            1000,12000,goal,120000000,1000,250,0,500,1000,0,hitch,9);
    };
    InputPercentileBuffer probe;probe.reset(6000);
    for(uint64_t f=1;f<=200;++f) observe(probe,f,9000);
    check(probe.stats().currentUs==5900,
          "a speculative release spends only one 100 us trial before fresh tail feedback");
    const auto held=probe.stats().currentUs;
    for(uint64_t f=201;f<=220;++f) observe(probe,f,9000,false,false);
    check(probe.stats().currentUs==held,"invalid input cannot fund another release trial");
    for(uint64_t f=221;f<=450;++f) observe(probe,f,9000);
    check(probe.stats().currentUs<held && probe.stats().currentUs>=5700,
          "fresh observations eventually authorize another finite release instead of a permanent hold");

    InputPercentileBuffer excess;excess.reset(6000);
    uint64_t previous=6000;
    for(uint64_t f=1;f<=800;++f) {
        observe(excess,f,2500);
        const auto current=excess.stats().currentUs;
        check(current<=previous && previous-current<=5,
              "measured excess drains no faster than tolerance per second");
        previous=current;
    }
    check(excess.stats().currentUs<3000,
          "measured excess does not wait for hundreds of speculative release trials");

    InputPercentileBuffer recovery;recovery.reset(1000);
    for(uint64_t f=1;f<=399;++f) observe(recovery,f,9000,f==10 || f==399);
    check(recovery.stats().recentMissed==2 && recovery.stats().targetUs==8500,
          "a fresh goal-breaking hitch authorizes growth to its measured requirement");
    observe(recovery,400,9000);
    const auto crossing=recovery.stats();
    check(crossing.recentMissed*200==crossing.recentEligible &&
          crossing.targetUs==8500 && crossing.currentUs<crossing.targetUs,
          "a percentage boundary cannot revoke a freshly justified growth ramp");
    const auto before=crossing.currentUs;
    observe(recovery,401,9000);
    check(recovery.stats().currentUs==before+5,
          "growth continues at tolerance per second after the percentage recovers");
    for(uint64_t f=402;f<=600;++f) observe(recovery,f,9000);
    check(recovery.stats().targetUs<=recovery.stats().currentUs,
          "old misses cannot sustain growth indefinitely after fresh hitch pressure ends");

    InputPercentileBuffer below;below.reset(6000);
    for(uint64_t f=1;f<=200;++f) observe(below,f,6500,f<=10, true,999000);
    check(below.stats().recentMissed==0,"a covered requirement does not invent misses");
    // A legacy history can remain below goal after its current protection is
    // sufficient. That statistic must not prohibit a bounded fresh trial.
    InputPercentileBuffer repaired;repaired.reset(1000);
    for(uint64_t f=1;f<=600;++f) observe(repaired,f,f<=10?2500:9000,f<=10);
    check(repaired.stats().recentMissed==10 && repaired.stats().currentUs<2000,
          "clean feedback permits a bounded trial despite unrepaired historical coverage");
}
void testAsymmetricLayerBufferMovement() {
    const auto observe=[](InputPercentileBuffer& p,uint64_t f) {
        p.observe(f,1000000+f*10000,2000000+f*10000,9000,true,
            1000,12000,995000,120000000,1000,250,0,500,1000,0,false,9);
    };
    InputPercentileBuffer down;down.reset(1000);observe(down,1);
    down.transitionFrom(6000);
    check(down.stats().currentUs==6000 && down.stats().targetUs==1000,
          "a lower layer selects its saved target without snapping applied protection down");
    for(uint64_t f=2;f<=11;++f) {
        const auto before=down.stats().currentUs;
        observe(down,f);
        down.advance(1000000+f*10000+1000);
        down.advance(1000000+f*10000+2000);
        check(before-down.stats().currentUs==500,
              "a downward layer transition drains exactly T per fresh frame, never per decision call");
    }
    check(down.stats().currentUs==1000,"the accelerated drain ends at the saved low-layer queue");
    InputPercentileBuffer up;up.reset(6000);observe(up,1);up.transitionFrom(1000);
    check(up.stats().currentUs==6000,"a substantial upward layer return restores protection immediately");
    InputPercentileBuffer small;small.reset(1300);observe(small,1);small.transitionFrom(1000);
    check(small.stats().currentUs==1000,"a small upward layer difference does not snap");
    observe(small,2);
    check(small.stats().currentUs==1005,"a small layer difference uses the ordinary T-per-second rate");
    InputPercentileBuffer cap;cap.reset(6000);observe(cap,1);
    cap.advance(1020000,1000,2000,995000,120000000,1000,250,0,500,1000,0,9);
    check(cap.stats().currentUs>=5995,"a shrinking maximum cannot bypass the downward slew limit");
    InputPercentileBuffer stored;stored.reset(1000);observe(stored,1);stored.transitionFrom(6000);
    stored.prepareForLayerStorage();
    check(stored.stats().currentUs==1000,"an interrupted drain stores that layer's own target, not inherited excess");
}
// Policy 10 keeps the layer rework but restores the ordinary policy-8 drain.
void testNormalDrainAfterLayerRework() {
    for (const uint64_t period : {8333, 33333}) {
        InputPercentileBuffer normal, corrected, restricted;
        normal.reset(6000); corrected.reset(6000); restricted.reset(6000);
        for (uint64_t f=1; f<=240; ++f) {
            for (auto pair : {std::pair<InputPercentileBuffer*,uint64_t>{&normal,8},
                              {&corrected,10}, {&restricted,9}})
                pair.first->observe(f,1000000+f*period,2000000+f*period,9000,true,
                    1000,12000,995000,120000000,1000,250,0,500,1000,0,false,pair.second);
            check(corrected.stats().currentUs==normal.stats().currentUs &&
                  corrected.stats().targetUs==normal.stats().targetUs,
                  "steady healthy release and target match the normal pre-9 law at both source rates");
        }
        check(corrected.stats().currentUs==1000 && restricted.stats().currentUs>5000,
              "healthy protection drains in seconds instead of waiting for repeated 200-frame trials");
    }
    const auto observe=[](InputPercentileBuffer& p,uint64_t f,int64_t required,
                          bool hitch=false,bool valid=true) {
        p.observe(f,1000000+f*10000,2000000+f*10000,required,valid,
            1000,12000,995000,120000000,1000,250,0,500,1000,0,hitch,10);
    };
    InputPercentileBuffer down, normal;
    down.reset(1000); normal.reset(6000);
    observe(down,1,9000); observe(normal,1,1500);
    down.transitionFrom(6000);
    check(down.stats().currentUs==6000 && down.stats().targetUs==1000,
          "a lower layer retains the old buffer and immediately reports the learned destination");
    uint64_t previous=6000;
    for (uint64_t f=2; f<=200; ++f) {
        observe(down,f,9000); observe(normal,f,1500);
        check(down.stats().currentUs==normal.stats().currentUs,
              "a decreasing layer uses the exact ordinary drain without a T-per-frame shortcut");
        check(previous-down.stats().currentUs<=50,
              "a decreasing layer stays inside normal 0.5-percent-per-interval release");
        const auto current=down.stats().currentUs;
        down.advance(1000000+f*10000+1000);
        down.advance(1000000+f*10000+2000);
        // Mirror decision-time calls: neither path may spend an interval twice.
        normal.advance(1000000+f*10000+1000);
        normal.advance(1000000+f*10000+2000);
        check(down.stats().currentUs==normal.stats().currentUs && current-down.stats().currentUs<=50,
              "repeated layer-drain decisions preserve the ordinary interval allowance");
        previous=down.stats().currentUs;
    }
    check(down.stats().currentUs==1000,"a normal downward transition reaches its saved target promptly");
    InputPercentileBuffer up; up.reset(6000); observe(up,1,9000); up.transitionFrom(1000);
    check(up.stats().currentUs==6000,"only an increasing learned layer can restore a large buffer immediately");
    InputPercentileBuffer small; small.reset(1300); observe(small,1,9000); small.transitionFrom(1000);
    observe(small,2,9000);
    check(small.stats().currentUs==1010,"small increases retain the normal configured rise rate");
    InputPercentileBuffer growth;growth.reset(1000);
    for(uint64_t f=1;f<=399;++f) observe(growth,f,9000,f==10 || f==399);
    const auto before=growth.stats().currentUs;
    observe(growth,400,9000);
    check(growth.stats().targetUs>growth.stats().currentUs && growth.stats().currentUs==before+10,
          "a fresh miss keeps normal growth responsive across the percentage boundary");
    const auto validCount=growth.stats().eligible;
    const auto beforeInvalid=growth.stats().currentUs;
    observe(growth,401,9000,false,false);
    check(growth.stats().eligible==validCount && growth.stats().currentUs==beforeInvalid,
          "invalid input cannot fund faster growth");
    Vrr13::ExtraBufferSlew extra;
    extra.begin(0,1000000,6000,500,1000,false,false,false);
    for (uint64_t f=1;f<=100;++f) {
        extra.begin(f,1000000+f*10000,6000-f*50,500,1000,false,false,false);
        extra.smoothing(0);
    }
    check(extra.smoothing(0)==500,
          "normal input release cannot accumulate debt that freezes extra-delay recovery");
}
void testStableTargetKeepsPreventableMissScoring() {
    const auto observe=[](InputPercentileBuffer& p,uint64_t f,int64_t required,
                          bool hitch=false,bool valid=true) {
        p.observe(f,1000000+f*10000,2000000+f*10000,required,valid,
            1000,12000,995000,120000000,1000,250,0,500,1000,0,hitch,10);
    };
    InputPercentileBuffer stable, probing;
    stable.reset(6000);stable.setStableAdaptation(true);probing.reset(6000);
    for(uint64_t f=1;f<=400;++f) {observe(stable,f,9000);observe(probing,f,9000);}
    check(stable.stats().currentUs==6000 && stable.stats().targetUs==6000 &&
          probing.stats().currentUs==1000,
          "stable adaptation retains the learned plateau instead of repeatedly probing below it");
    check(stable.stats().missed==0 && probing.stats().missed==0 && stable.stats().eligible==400,
          "a raw readiness overrun without a matching hitch never counts as a preventable miss");
    const auto eligible=stable.stats().eligible;
    observe(stable,401,18000,true);
    check(stable.stats().eligible==eligible && stable.stats().missed==0 && stable.stats().currentUs==6000,
          "work beyond the selected buffer maximum stays excluded even when it hitches");
    observe(stable,402,9000,true);observe(stable,403,9000,true);observe(stable,404,9000,true);
    check(stable.stats().missed==3 && stable.stats().targetUs==8500 &&
          stable.stats().currentUs>6000 && stable.stats().currentUs<=6030,
          "fresh preventable hitches fund protection with normal bounded growth rather than a jump");
    const auto before=stable.stats().currentUs;
    observe(stable,405,9000,false,false);
    check(stable.stats().currentUs==before && stable.stats().missed==3,
          "invalid readiness cannot grow protection or turn into successful feedback");
    InputPercentileBuffer down;down.reset(1000);down.setStableAdaptation(true);
    observe(down,1,9000);down.transitionFrom(6000);
    check(down.stats().currentUs==6000 && down.stats().targetUs==1000,
          "a lower layer retains applied protection and publishes its learned destination");
    for(uint64_t f=2;f<=101;++f) observe(down,f,9000);
    check(down.stats().currentUs==5750 && down.stats().effectiveFallRateUsPerSecond==250,
          "lower layers use the historical 250 us/s drain without an accelerated layer shortcut");
    InputPercentileBuffer up;up.reset(6000);up.setStableAdaptation(true);
    observe(up,1,9000);up.transitionFrom(1000);
    check(up.stats().currentUs==6000,"larger learned layers still restore their existing protection immediately");
}
// Revision 2 restores progress without turning a clean raw-input plateau into
// an unrestricted release ramp. Existing revision-1 expectations stay above.
void testFeedbackValidatedStableRelease() {
    const auto observe=[](InputPercentileBuffer& p,uint64_t f,int64_t required,
                          bool hitch=false,bool valid=true,uint64_t room=1000) {
        p.observe(f,1000000+f*10000,2000000+f*10000,required,valid,
            1000,12000,995000,120000000,1000,250,0,500,room,0,hitch,10);
    };
    InputPercentileBuffer trial;trial.reset(8621);trial.setStableAdaptation(2);
    for(uint64_t f=1;f<=200;++f) observe(trial,f,11000);
    check(trial.stats().currentUs==8521 && trial.stats().missed==0,
          "a clean raw-input plateau permits one finite 100 us trial, not a continuous drain");
    const auto held=trial.stats().currentUs;
    for(uint64_t f=201;f<=500;++f) observe(trial,f,11000,false,false);
    check(trial.stats().currentUs==held && trial.stats().eligible==200,
          "invalid outcomes cannot fund the tail support needed for another trial");
    for(uint64_t f=501;f<=800;++f) observe(trial,f,11000);
    check(trial.stats().currentUs<held && trial.stats().currentUs>=held-200,
          "fresh feedback at the lower buffer permits another bounded trial");
    const auto before=trial.stats();
    observe(trial,801,11000,true);
    check(trial.stats().missed==before.missed+1 && trial.stats().currentUs>=before.currentUs &&
          trial.stats().currentUs<=before.currentUs+10,
          "a fresh preventable hitch cancels release and retains normal bounded growth");
    check(trial.stats().probabilityPpm==995000 && trial.stats().windowUs==120000000,
          "feedback trials preserve the selected smoothness target and full learning window");
    InputPercentileBuffer measured,recorded,noRoom;
    measured.reset(8621);measured.setStableAdaptation(2);
    recorded.reset(8621);recorded.setStableAdaptation(1);
    noRoom.reset(8621);noRoom.setStableAdaptation(2);
    uint64_t previous=8621;
    for(uint64_t f=1;f<=250;++f) {
        observe(measured,f,1500);observe(recorded,f,1500);observe(noRoom,f,9500,false,true,0);
        const auto now=measured.stats().currentUs;
        check(now<=previous && previous-now<=50,
              "measured surplus obeys the normal 0.5-percent-per-interval recovery bound");
        previous=now;
        if(f==100) {
            const auto saved=now;
            auto delayed=measured;delayed.advance(2010000);delayed.advance(2020000);
            check(saved-delayed.stats().currentUs<=50,
                  "repeated decisions cannot spend the same interval allowance twice");
        }
    }
    check(measured.stats().currentUs==recorded.stats().currentUs && recorded.stats().currentUs>7900,
          "ordinary measured-surplus release remains identical to the configured 250 us/s historical law");
    check(noRoom.stats().currentUs==8621,
          "a plateau without physical recovery room cannot authorize speculative release");
    // An interrupted lower-layer visit must save its own learned destination.
    InputPercentileBuffer lower;lower.reset(1000);lower.setStableAdaptation(2);
    // Support the selected tail with this layer's own low requirements first.
    observe(lower,1,9500);
    for(uint64_t f=2;f<=201;++f) observe(lower,f,1500);
    lower.transitionFrom(8621);
    for(uint64_t f=202;f<=301;++f) observe(lower,f,1500);
    check(lower.stats().currentUs==8371,
          "a lower layer retains the configured normal drain before storing its own learned destination");
    lower.prepareForLayerStorage();
    check(lower.stats().currentUs==1000 && lower.stats().targetUs==1000,
          "a layer cache cannot turn inherited excess into a permanent learned floor");
}


// Revision 3 releases continuously using elapsed time, not a frame-count wait.
void testContinuousStableRelease() {
    for (const uint64_t revision : {3, 4})
    for (const uint64_t fps : {20, 30, 116}) {
        for (const uint64_t probability : {990000, 995000, 999500}) {
            InputPercentileBuffer buffer;
            buffer.reset(8621);
            buffer.setStableAdaptation(revision);
            const auto observe = [&](uint64_t f, bool valid = true, bool miss = false,
                                     uint64_t room = 1000) {
                const auto at = 1000000 + f * 1000000 / fps;
                buffer.observe(f, at, at + 1000000, 11000, valid,
                    1000, 12000, probability, 120000000, 1000, 250,
                    0, 500, room, 0, miss, 10);
            };
            for (uint64_t f = 1; f <= fps * 2; ++f) observe(f);
            const auto start = buffer.stats();
            for (uint64_t f = fps * 2 + 1; f <= fps * 6; ++f) observe(f);
            const auto end = buffer.stats();
            const auto released = start.currentUs - end.currentUs;
            std::fprintf(stderr, "continuous release %llu FPS p%llu: %llu us / 4s\n",
                (unsigned long long)fps, (unsigned long long)probability,
                (unsigned long long)released);
            check(released >= 950 && released <= 1000,
                "clean plateau drains at the configured time-based rate without tail-sample pauses");
            check(end.missed == 0 && end.probabilityPpm == probability && end.windowUs == 120000000,
                "continuous release preserves actual miss scoring and selected history");
            const auto beforeInvalid = end.currentUs;
            for (uint64_t f = fps * 6 + 1; f <= fps * 7; ++f) observe(f, false);
            check(buffer.stats().currentUs == beforeInvalid,
                "invalid observations cannot authorize continuous plateau release");
            for (uint64_t f = fps * 7 + 1; f <= fps * 8; ++f) observe(f, true, false, 0);
            check(buffer.stats().currentUs == beforeInvalid,
                "no physical recovery room cannot authorize plateau release");
            observe(fps * 8 + 1, true, true);
            check(buffer.stats().currentUs >= beforeInvalid && buffer.stats().missed == 1,
                "fresh qualifying misses still stop plateau release");
        }
    }
}


void testSelectedWindowReleaseGate() {
    for (const uint64_t fps : {20, 116}) {
        for (const int mode : {0, 1, 2}) {
            Vrr13::InputCoverageHistory history;
            constexpr uint64_t window = 120000000, probability = 995000;
            // One percent historical misses, all older than the short report.
            for (uint64_t f = 1; f <= 2000; ++f)
                history.observe(1000000 + f * 10000, window, true, false, f <= 20);
            InputPercentileBuffer buffer;
            buffer.reset(mode == 2 ? 4000 : 8621);
            buffer.setStableAdaptation(4);
            buffer.endLearning();
            uint64_t frame = 0;
            const int64_t required = mode == 0 ? 11000 : 2000;
            const auto observe = [&](uint64_t at) {
                history.advance(at, window);
                const auto before = buffer.stats();
                buffer.observe(++frame, at, at + 1000000, required, true,
                    1000, 12000, probability, window, 1000, 250,
                    0, 500, 1000, 0, false, 10, !history.belowGoal(probability));
                const auto after = buffer.stats();
                history.observe(at, window, after.eligible > before.eligible,
                    after.absorbed > before.absorbed, after.missed > before.missed);
            };
            observe(22000000);
            if (mode == 2) buffer.transitionFrom(8621);
            const auto held = buffer.stats().currentUs;
            for (uint64_t f = 1; f <= fps * 4; ++f) {
                const uint64_t at = 22000000 + f * 1000000 / fps;
                observe(at);
                buffer.advance(at + 1, 1000, 12000, probability, window,
                    1000, 250, 0, 500, 1000, 0, 10, !history.belowGoal(probability));
                check(buffer.stats().currentUs == held,
                    "below selected session goal blocks observation and decision release, including lower demand and layers");
            }
            auto coverage = buffer.stats();
            history.report(coverage, true);
            check(coverage.reportingEligible && coverage.reportingMissed == 0 &&
                  history.belowGoal(probability),
                "a perfect ten-second report cannot override a failing 120-second goal");
            check(buffer.stats().recentMissed == 0 && buffer.stats().recentReleaseMisses == 0,
                "clean active-layer and one-second feedback cannot erase session misses");
            // Misses expire by wall time. Resume with a fresh pair, without
            // waiting for 200 observations or banking the held time as credit.
            observe(122000000);
            const auto resumed = buffer.stats().currentUs;
            check(!history.belowGoal(probability) && held - resumed <= 25,
                "expired session misses reopen release without accumulated hold credit");
            for (uint64_t f = 1; f <= fps; ++f)
                observe(122000000 + f * 1000000 / fps);
            const auto released = resumed - buffer.stats().currentUs;
            // After the long gap, plateau probes still need eight fresh
            // eligible samples. Even at 20 FPS they resume inside this second.
            check(released >= 125 && released <= 250,
                "goal recovery resumes bounded continuous release within one second, without 200 frames");
        }
    }
    Vrr13::InputCoverageHistory boundary;
    check(!boundary.belowGoal(995000), "empty session history preserves ordinary unknown-history expiry");
    for (uint64_t f = 1; f <= 199; ++f)
        boundary.observe(f * 10000, 120000000, true, false, f == 1);
    check(boundary.belowGoal(995000), "one miss in 199 remains below 99.5 percent");
    boundary.observe(2000000, 120000000, false, false, false);
    check(boundary.belowGoal(995000), "invalid input cannot supply clean goal credit");
    boundary.observe(2010000, 120000000, true, false, false);
    check(!boundary.belowGoal(995000), "exactly 99.5 percent authorizes release without an extra wait");
}


void testRequiredBufferEstimateMath() {
    check(Vrr13::InputBufferEstimate::calibratedProbability(995000,10000,50)==995000 &&
          Vrr13::InputBufferEstimate::calibratedProbability(995000,10000,0)==995000,
        "meeting the actual goal keeps the user's modeled miss allowance rather than demanding zero hitches");
    check(Vrr13::InputBufferEstimate::calibratedProbability(995000,10000,100)==997500 &&
          Vrr13::InputBufferEstimate::calibratedProbability(995000,10000,200)==998750,
        "full-window goal deficits tighten modeled allowance proportionally without a hard hold");
    struct Sample { uint64_t at,slot; int64_t required; uint64_t epoch;
                    bool seed; uint64_t recoveryUs; bool missed = false; };
    std::vector<Sample> samples;
    for (uint64_t f=0;f<180;++f) {
        const int64_t q = f<25 ? 3500 : f%17==0 ? 9000 : f%7==0 ? 4800 : 1000;
        samples.push_back({1000000+f*10000,2000000+f*10000,q,
                          f/60,false,f%3==0?150u:500u});
    }
    for (const uint64_t tolerance : {250,500,2000}) {
        for (const uint64_t probability : {900000,990000,995000,999900}) {
            Vrr13::InputBufferEstimate estimate;
            estimate.evaluate(samples,probability,tolerance);
            for (const int64_t projection : {-1200,0,1700}) {
                for (uint64_t b=0;b<=12000;b+=137) {
                    uint64_t misses=0,previousLate=0,epoch=UINT64_MAX;
                    for (const auto& s:samples) {
                        const uint64_t ready=s.required>projection?uint64_t(s.required-projection):0;
                        const uint64_t inputLate=ready>b?ready-b:0;
                        if (s.epoch!=epoch) {previousLate=inputLate;epoch=s.epoch;continue;}
                        const uint64_t recovered=previousLate-std::min(previousLate,s.recoveryUs);
                        const uint64_t late=std::max(inputLate,recovered);
                        misses+=inputLate>tolerance && late>previousLate && late-previousLate>tolerance;
                        previousLate=late;
                    }
                    check(estimate.missesAt(b,samples.back().at,300000000,projection,0,tolerance)==misses,
                        "candidate threshold math matches an independent constant-buffer cadence simulation");
                }
                const auto target=estimate.requiredUs(projection,0,tolerance);
                const auto allowed=estimate.samples()*(1000000-probability)/1000000;
                check(estimate.missesAt(target,samples.back().at,300000000,projection,0,tolerance)<=allowed,
                    "selected percentile covers the configured share of modeled intervals");
                if(target) check(estimate.missesAt(target-1,samples.back().at,300000000,projection,0,tolerance)>allowed,
                    "required-buffer estimate is the smallest value satisfying the selected goal");
            }
        }
    }
    std::vector<Sample> constant;
    for(uint64_t f=0;f<500;++f)constant.push_back({1000000+f*10000,2000000+f*10000,9000,1,false,200});
    Vrr13::InputBufferEstimate estimate;estimate.evaluate(constant,995000,500);
    check(estimate.requiredUs(0,0,500)==0 && estimate.missesAt(0,6000000,120000000,0,0,500)==0,
        "constant late input with constant cadence cannot manufacture recurring buffer demand");
    for(size_t i=25;i<constant.size();i+=30)constant[i].missed=true;
    estimate.evaluate(constant,995000,500);
    check(estimate.requiredUs(0,0,500)==8500 &&
          estimate.missesAt(1000,6000000,120000000,0,0,500)>2,
        "proven readiness-attributed hitches override modeled recovery that would otherwise hide demand");
    for(auto& s:constant)s.seed=true;
    estimate.evaluate(constant,995000,500);
    check(estimate.samples()==0,"copied layer seeds are not fresh modeled clean outcomes");
}

void testRollingEstimateSettingsAndConvergence() {
    for(const uint64_t seconds:{10,30,60,120,300}) {
        InputPercentileBuffer buffer;buffer.reset(9000);buffer.setStableAdaptation(5);
        for(uint64_t f=1;f<=200;++f) {
            const auto at=1000000+f*10000;
            buffer.observe(f,at,at+1000000,f%10==0?8000:0,true,1000,12000,
                995000,seconds*1000000,1000,250,0,500,1000,0,false,10,false);
        }
        const auto before=buffer.stats();
        check(before.windowUs==seconds*1000000 && before.probabilityPpm==995000 && before.targetUs==7500,
            "rolling demand uses the user's history seconds and goal, not a fixed 120-second window");
        check(before.currentUs<9000 && before.currentUs>7500,
            "gradual convergence releases toward the estimate even when the old session hard-hold flag is false");
        buffer.advance(3100000,1000,12000,995000,seconds*1000000,
            1000,250,3000,500,1000,100000,10,false);
        check(buffer.stats().targetUs==7500,
            "later clock/budget projection cannot erase the protection duration retained frames needed");
        buffer.advance(15000000,1000,12000,995000,seconds*1000000,1000,250,0,500,1000,0,10,false);
        check(seconds==10?buffer.stats().targetUs==1000:buffer.stats().targetUs==7500,
            "wall-clock expiry respects each selected history duration");
        buffer.advance((seconds+4)*1000000,1000,12000,995000,seconds*1000000,1000,250,0,500,1000,0,10,false);
        check(buffer.stats().expiredUnknown && buffer.stats().targetUs==1000,
            "expired delivery evidence retires at the configured horizon");
    }
    {
        InputPercentileBuffer layer;layer.reset(5000);layer.setStableAdaptation(5);
        for(uint64_t f=1;f<=200;++f) {
            const uint64_t at=1000000+f*10000;
            layer.observe(f,at,at+1000000,f%10==0?8000:0,true,
                1000,12000,995000,120000000,1000,250,0,500,1000,0,false,10);
            if(f==1)layer.transitionFrom(9000);
        }
        const auto destination=layer.stats().targetUs;
        check(layer.stats().currentUs>destination && destination==7500,
            "layer regression has updated rolling demand while inherited protection is draining");
        layer.prepareForLayerStorage();
        check(layer.stats().currentUs==destination,
            "saved layer retains its updated computed destination rather than an obsolete floor or inherited excess");
    }
    for(const uint64_t fps:{20,30,116}) {
        InputPercentileBuffer buffer;buffer.reset(9000);buffer.setStableAdaptation(5);
        uint64_t firstSecond=0,atSix=0;
        for(uint64_t f=1;f<=fps*7;++f) {
            const auto at=1000000+f*1000000/fps;
            buffer.observe(f,at,at+1000000,f%10==0?8000:0,true,1000,12000,
                995000,120000000,1000,250,0,500,1000,0,false,10,false);
            if(f==fps)firstSecond=9000-buffer.stats().currentUs;
            if(f==fps*6)atSix=buffer.stats().currentUs;
        }
        const auto end=buffer.stats();
        check(firstSecond>150 && firstSecond<=250,
            "convergence starts within one second at low FPS without waiting for 200 frames");
        check(atSix-end.currentUs>0 && atSix-end.currentUs<firstSecond && end.currentUs>7500,
            "movement tapers near the rolling estimate instead of draining at a fixed speed or overshooting");
    }
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
void feed(InputPercentileBuffer& p,uint64_t frame,uint64_t at,uint64_t slot,
          int64_t required,bool valid=true,uint64_t probability=990000,
          uint64_t window=30000000,int64_t budget=0,uint64_t low=0,uint64_t high=6000,uint64_t tolerance=0) {
    p.observe(frame,at,slot,required,valid,low,high,probability,window,1000,250,budget,tolerance);
}
// The user's design: a lower frame rate frees VRR headroom (source period minus
// display period/render), which absorbs lateness instead of extra buffer.
void testVrrHeadroomAbsorbsLateness() {
    const auto observe=[](InputPercentileBuffer& p,uint64_t frame,int64_t required,uint64_t headroom) {
        p.observe(frame,frame*33333,1000000+frame*33333,required,true,1000,25000,995000,200000000,
                  1000000,250,0,750,headroom);
    };
    // Same 9 ms input tail at 30 FPS (25 ms headroom) and at 110 FPS (0.76 ms).
    InputPercentileBuffer low,high;low.reset(1000);high.reset(1000);
    for(uint64_t f=1;f<=400;++f) {
        const int64_t required=f%50==0?9000:2000;
        observe(low,f,required,25000);observe(high,f,required,760);
    }
    check(low.stats().targetUs==1000 && low.stats().missed==0,
          "30 FPS headroom covers a 9 ms tail: target stays at the floor and nothing is a miss");
    check(high.stats().targetUs==9000-750-760 && high.stats().missed>0,
          "110 FPS has little headroom: the same tail needs buffer");
    check(low.stats().currentUs<=high.stats().currentUs,"30 FPS never holds more buffer than 110 FPS for the same jitter");
    // Protection learned at 110 FPS is credited by the headroom gained at 30 FPS.
    InputPercentileBuffer change;change.reset(9000);
    change.creditHeadroom(25000-760);
    check(change.stats().currentUs==1000 || change.stats().currentUs==0,
          "rate change that frees headroom credits it immediately instead of freezing old protection");
    InputPercentileBuffer floor;floor.reset(9000);
    floor.observe(1,33333,1033333,2000,true,1000,25000,995000,200000000,1000000,250,0,750,760);
    floor.creditHeadroom(3000);
    check(floor.stats().currentUs==6000,"credit removes exactly the gained headroom above the floor");
}
void testPhaseBreakKeepsEvidenceAndHeadroomLossRestores() {
    const auto observe=[](InputPercentileBuffer& p,uint64_t frame,int64_t required,uint64_t headroom) {
        p.observe(frame,frame*8333,1000000+frame*8333,required,true,1000,12000,995000,200000000,
                  1000000,250,0,500,headroom);
    };
    InputPercentileBuffer p;p.reset(1000);
    for(uint64_t f=1;f<=400;++f) observe(p,f,f%50==0?7000:2000,0);
    const auto before=p.stats();
    p.breakAdjacency();
    const auto after=p.stats();
    check(after.samples==before.samples && after.recentEligible==before.recentEligible &&
          after.recentMissed==before.recentMissed && after.targetUs==before.targetUs && after.epochResets==before.epochResets,
          "a phase discontinuity keeps evidence, coverage outcomes and target");
    observe(p,402,2000,0);
    check(p.stats().samples==before.samples+1,"observation after a phase break extends the same history");
    // Headroom loss steps current to the evidence-backed target, bounded by the loss.
    InputPercentileBuffer q;q.reset(1000);
    for(uint64_t f=1;f<=400;++f) observe(q,f,f%50==0?7000:2000,0);
    q.creditHeadroom(25000);
    check(q.stats().currentUs==1000,"headroom gained at 30 FPS credits protection to the floor");
    q.advance(400*8333+1,1000,12000,995000,200000000,1000000,250,0,500,0);
    const auto target=q.stats().targetUs;
    q.raiseForHeadroomLoss(2000);
    check(q.stats().currentUs==3000,"a small headroom loss raises current by exactly the loss");
    q.raiseForHeadroomLoss(25000);
    check(q.stats().currentUs==target && target==6500,"a large loss stops at the evidence-backed target");
    q.raiseForHeadroomLoss(25000);
    check(q.stats().currentUs==target,"current never exceeds the target through a headroom loss");
    // A new rate layer is seeded with copied evidence: it sets the target, but
    // the other rate's outcomes leave coverage and expire without underflow.
    InputPercentileBuffer seeded=p;seeded.markEvidenceAsSeed();
    check(seeded.stats().recentEligible==0 && seeded.stats().recentMissed==0 &&
          seeded.stats().samples==p.stats().samples,"seeded evidence keeps samples but no coverage outcomes");
    observe(seeded,403,2000,0);
    check(seeded.stats().recentEligible==1 && seeded.stats().targetUs==p.stats().targetUs,
          "the new layer's own frames are its coverage while the seed still sets the target");
    seeded.advance(403*8333+200000001,1000,12000,995000,200000000,1000000,250,0,500,0);
    check(seeded.stats().recentEligible==0 && seeded.stats().samples==0,"expired seed and own samples leave counts consistent");
}
void testOnlyPreventableHitchesScoreAndGrow() {
    const auto observe=[](InputPercentileBuffer& p,uint64_t f,int64_t required,bool hitch,
                          uint64_t probability=995000) {
        p.observe(f,1000000+f*10000,1000000+f*10000,required,true,
                  1000,6000,probability,10000000,1000,250,0,500,0,0,hitch,true);
    };
    InputPercentileBuffer p;p.reset(1000);
    for(uint64_t f=1;f<=500;++f) observe(p,f,5000,false);
    check(p.stats().eligible==500 && p.stats().missed==0 && p.stats().raises==0 &&
          p.stats().currentUs==1000 && p.stats().targetUs==1000,
          "late input without an actual hitch cannot reduce the goal or buy buffer, even while learning");
    for(uint64_t f=501;f<=520;++f) observe(p,f,3500,true);
    check(p.stats().missed>0 && p.stats().raises>0 && p.stats().currentUs>1000,
          "actual preventable input hitches beyond tolerance lower coverage and authorize growth");
    for(uint64_t f=521;f<=1000;++f) observe(p,f,6000,false);
    check(p.stats().targetUs<=3000 && p.stats().currentUs<=3000,
          "clean late inputs cannot enlarge earlier miss-driven demand while old misses still leave the goal below target");
    InputPercentileBuffer capped;capped.reset(1000);
    for(uint64_t f=1;f<=500;++f) observe(capped,f,10000,true);
    check(capped.stats().eligible==0 && capped.stats().ineligible==500 &&
          capped.stats().missed==0 && capped.stats().raises==0,
          "a spike beyond the permitted maximum is unknown for the preventable goal, never success or growth debt");
    InputPercentileBuffer atGoal;atGoal.reset(1000);
    for(uint64_t f=1;f<=200;++f) observe(atGoal,f,1000,false,990000);
    observe(atGoal,201,4500,true,990000);
    check(atGoal.stats().missed==1 && atGoal.stats().raises==0 && atGoal.stats().currentUs==1000,
          "a real miss is reported but cannot raise the buffer while the selected goal is still met");
    InputPercentileBuffer legacy;legacy.reset(1000);
    legacy.observe(1,1000000,1000000,5000,true,1000,6000,995000,10000000,1000,250,
                   0,500,0,0,false,false);
    check(legacy.stats().missed==1 && legacy.stats().currentUs>1000,
          "explicit historical policy preserves raw input-miss and startup growth semantics");
}

// A real miss that puts coverage below goal must not be trimmed away as a
// raw-input outlier, or forgotten on the next clean observation/decision.
void testMissProtectionSurvivesQuantileTrim() {
    InputPercentileBuffer p; p.reset(4000); p.setLearningTrim(1);
    const auto observe=[&](uint64_t f,int64_t required,bool hitch) {
        p.observe(f,1000000+f*10000,2000000+f*10000,required,true,
                  1000,12000,995000,10000000,1000,250,0,500,0,0,hitch,2);
    };
    for(uint64_t f=1;f<=20;++f) observe(f,1000,false);
    observe(21,7000,true);
    check(p.stats().missed==1 && p.stats().targetUs==6500 && p.stats().currentUs==6500,
          "a goal-breaking measured hitch overrides startup raw-input trimming");
    for(uint64_t f=22;f<=80;++f) {
        observe(f,11000,false);
        p.advance(1000000+f*10000+1);
    }
    check(p.stats().targetUs==6500 && p.stats().currentUs==6500,
          "qualified miss demand survives clean observations without clean late inputs raising it");
}

void testAppliedProtectionFollowsClockCoordinate() {
    // A proven transition hitch earns 10 ms of protection. As the producer
    // mapping catches up by 8 ms, the same source deadline needs only 2 ms:
    // slewing the buffer down at 250 us/s would count that mapping twice.
    for (uint64_t policy : {uint64_t(2), uint64_t(3)}) {
        InputPercentileBuffer p; p.reset(1000);
        p.observe(1,1000000,2000000,10500,true,1000,20000,995000,120000000,
                  1000,250,0,500,0,100000,true,policy);
        check(p.stats().currentUs==10000,"fixture earns protection from a qualifying hitch");
        p.advance(1000001,1000,20000,995000,120000000,1000,250,0,500,0,108000,policy);
        check(p.stats().targetUs==2000,"miss-backed target projects the source coordinate once");
        check(p.stats().currentUs==(policy==3?2000:10000),
              "new policy projects applied protection too; recorded policy keeps its historical slow release");
        p.advance(1000001,1000,20000,995000,120000000,1000,250,0,500,0,108000,policy);
        check(p.stats().currentUs==(policy==3?2000:10000),"repeated clock projection does not spend the movement twice");
        if(policy==3) {
            const auto saved = p;
            p.advance(1000001,1000,20000,995000,120000000,1000,250,0,500,0,100000,policy);
            check(p.stats().currentUs==10000 && p.stats().targetUs==10000,
                  "reversing the mapping restores the same source deadline without ratcheting");
            check(p.stats().missed==1 && p.stats().recentMissed==1,
                  "coordinate projection preserves actual historical miss outcomes");
            auto restored = saved;
            restored.advance(1000001,1000,20000,995000,120000000,1000,250,0,500,0,104000,policy);
            check(restored.stats().currentUs==6000 && restored.stats().targetUs==6000,
                  "a saved rate layer projects from its own last clock coordinate on restore");
            restored.advance(1000001,1000,5000,995000,120000000,1000,250,0,500,0,100000,policy);
            check(restored.stats().currentUs==5000,"clock projection obeys the selected maximum");
            restored.advance(1000001,1000,5000,995000,120000000,1000,250,0,500,0,120000,policy);
            check(restored.stats().currentUs==1000,"clock projection obeys the selected minimum");
        }
    }
}

void testClockReleaseRespectsMissGoal() {
    InputPercentileBuffer unknown; unknown.reset(10000);
    unknown.advance(1000000,1000,20000,995000,120000000,1000,250,0,500,0,100000,4);
    unknown.advance(1000001,1000,20000,995000,120000000,1000,250,0,500,0,108000,4);
    check(unknown.stats().currentUs==10000,
          "unknown coverage cannot authorize accelerated clock-driven release");
    InputPercentileBuffer p; p.reset(1000);
    p.observe(1,1000000,2000000,10500,true,1000,20000,995000,120000000,
              1000,250,0,500,0,100000,true,4);
    p.advance(1000001,1000,20000,995000,120000000,1000,250,0,500,0,108000,4);
    check(p.stats().currentUs==10000 && p.stats().targetUs==2000,
          "mapping cannot spend current protection while the actual miss goal is unmet");
    for(uint64_t f=2;f<=201;++f)
        p.observe(f,1000000+f*10000,2000000+f*10000,1000,true,
                  1000,20000,995000,120000000,1000,250,0,500,0,108000,false,4);
    const auto covered=p.stats();
    check(covered.recentEligible==201 && covered.recentMissed==1,
          "fixture recovers to the selected coverage without rewriting the original miss");
    p.advance(3010001,1000,20000,995000,120000000,1000,250,0,500,0,110000,4);
    check(p.stats().currentUs==covered.currentUs-2000,
          "once the actual goal is met, a forward mapping correction gives back its delay exactly once");
}

void testLiveMappingIsSeparateFromHistoryDrift() {
    InputPercentileBuffer p; p.reset(1000);
    p.observe(1,1000000,2000000,10500,true,1000,20000,995000,120000000,
              1000,250,0,500,0,100000,true,5);
    p.advance(1000001,1000,20000,995000,120000000,1000,250,0,500,0,102000,5);
    check(p.stats().currentUs==10000 && p.stats().targetUs==8000,
          "history drift changes the demand estimate, not the live applied protection");
    p.remapProtection(200000,208000);
    check(p.stats().currentUs==2000,
          "an actual 8 ms mapping advance returns 8 ms of applied delay without changing its source deadline");
    p.advance(1000001,1000,20000,995000,120000000,1000,250,0,500,0,110000,5);
    p.remapProtection(208000,208000);
    check(p.stats().currentUs==2000 && p.stats().missed==1,
          "repeated projection spends no extra delay and preserves observed misses");
    p.remapProtection(208000,200000);
    check(p.stats().currentUs==10000,"reversing the live mapping retains the same deadline without ratcheting");
    p.remapProtection(std::numeric_limits<int64_t>::min(),std::numeric_limits<int64_t>::max());
    check(p.stats().currentUs==1000,"forward remapping saturates safely at the selected minimum");
    p.remapProtection(std::numeric_limits<int64_t>::max(),std::numeric_limits<int64_t>::min());
    check(p.stats().currentUs==20000,"reverse remapping saturates safely at the selected maximum");
}

void testMatureMissAttackUsesOnlyMeasuredDemand() {
    InputPercentileBuffer p; p.reset(1000); p.endLearning();
    const auto observe=[&](uint64_t f,int64_t required,bool hitch) {
        p.observe(f,1000000+f*10000,2000000+f*10000,required,true,
                  1000,12000,995000,10000000,1000,250,0,500,0,0,hitch,2);
    };
    for(uint64_t f=1;f<=200;++f) observe(f,1000,false);
    observe(201,7000,true); // Still within the profile's permitted tail.
    check(p.stats().currentUs==1000,"a mature in-budget miss does not authorize an attack");
    observe(202,9000,true);
    check(p.stats().currentUs==6500,"a goal-breaking hitch immediately retains the justified miss quantile, not the largest tail");
    const auto protectedUs=p.stats().currentUs;
    observe(203,11000,false);
    check(p.stats().currentUs==protectedUs && p.stats().targetUs==protectedUs,
          "a harmless late frame cannot trigger an immediate attack or buy extra protection");
    InputPercentileBuffer clock; clock.reset(4000); clock.endLearning();
    for(uint64_t f=1;f<=21;++f)
        clock.observe(f,1000000+f*10000,2000000+f*10000,f==21?7000:1000,true,
                      1000,12000,995000,10000000,1000,250,0,500,0,0,f==21,2);
    clock.advance(1220000,1000,12000,995000,10000000,1000,250,0,500,0,1000,2);
    check(clock.stats().targetUs==5500,"miss-backed protection projects a source-clock change exactly once");
    clock.advance(1230000,1000,12000,995000,10000000,1000,250,500,500,0,1000,2);
    check(clock.stats().targetUs==5000,"miss-backed protection applies the readiness budget exactly once");
}

void testQuantileAndClock() {
    InputPercentileBuffer p;
    check(p.demand(1600)==1600,"uninitialized helper preserves caller delay");
    p.reset(1600);
    feed(p,0,10000,10000,1000);
    check(p.stats().eligible==1 && p.stats().absorbed==1,"frame zero is eligible and absorbed positive input is counted");
    p.reset(1600);
    for(uint64_t i=1;i<=99;++i) feed(p,i,i*100000,1000000000+i*10000,i==1?2000:100);
    check(p.stats().tailUncertain && p.stats().targetUs==2000,"unsupported 99 percent tail uses observed maximum");
    feed(p,100,10000000,1001000000,100);
    check(!p.stats().tailUncertain && p.stats().targetUs==100,"nearest-rank supported 99 percent quantile");
    feed(p,101,40000000,1001010000,50);
    check(p.stats().samples==1 && p.stats().targetUs==50,"history ages by observation clock even when source slots hardly advance");
    check(p.stats().windowUs==30000000,"explicit thirty-second requested history is preserved");
    feed(p,102,40100000,1001020000,50,true,990000,1);
    check(p.stats().windowUs==10000000,"short history is clamped to the supported ten-second minimum");
    feed(p,103,40200000,1001030000,50,true,990000,900000000);
    check(p.stats().windowUs==300000000,"history is bounded at five minutes");
}
void testBudgetAndSignedOffsets() {
    InputPercentileBuffer p;p.reset(1600);
    feed(p,1,1000000,900000,1000,true,995000,30000000,200);
    check(p.stats().rawRequiredQuantileUs==1200 && p.stats().targetUs==1000,"raw signed source headroom retains readiness budget");
    feed(p,2,1008333,908333,600,true,995000,30000000,600);
    check(p.stats().targetUs==600 && p.stats().samples==2,"budget rise renormalizes retained history without reset or false tail");
    feed(p,3,1016666,916666,1100,true,995000,30000000,100);
    check(p.stats().targetUs==1100 && p.stats().samples==3,"budget fall renormalizes history without manufacturing new requirement");
    p.breakSequence();feed(p,4,1020000,920000,1500,true,990000,30000000,-500);
    check(p.stats().rawRequiredQuantileUs==1000 && p.stats().targetUs==1500,"negative readiness budget preserves signed source headroom");
    p.breakSequence();feed(p,4,1024999,924999,-600,true,990000,30000000,200);
    check(p.stats().rawRequiredQuantileUs==-400 && p.stats().targetUs==0,"negative raw headroom stays signed and requests no buffer");
    feed(p,5,1026000,926000,200,true,990000,30000000,-600);
    check(p.stats().rawRequiredQuantileUs==-400 && p.stats().targetUs==200,
        "negative quantile and more negative budget yield positive headroom without unsigned conversion artifacts");
    p.breakSequence();feed(p,5,1027000,927000,std::numeric_limits<int64_t>::min(),true,990000,30000000,-200);
    check(p.stats().rawRequiredQuantileUs==std::numeric_limits<int64_t>::min() && p.stats().targetUs==0,
        "signed negative normalization overflow saturates safely");
    p.breakSequence();feed(p,5,1033332,933332,std::numeric_limits<int64_t>::max(),true,990000,30000000,200);
    check(p.stats().rawRequiredQuantileUs==std::numeric_limits<int64_t>::max() && p.stats().clipped,"signed positive overflow saturates safely");
}
void testInputToleranceBoundaryAndNormalization() {
    constexpr uint64_t B=5000,T=750;
    for(const auto excess:{690ULL,750ULL,751ULL}) {
        InputPercentileBuffer p;p.reset(B);
        feed(p,1,1000000,1000000,int64_t(B+excess),true,995000,200000000,0,0,10000,T);
        auto stats=p.stats();
        check(stats.toleranceUs==T && stats.rawRequiredQuantileUs==int64_t(B+excess),
            "input tolerance is explicit and does not alter retained raw headroom");
        check(stats.targetUs==B+excess-T,
            "percentile buffer demand subtracts tolerance exactly once");
        check((stats.missed>0)==(excess>T),
            "690us and exactly750us beyond applied buffer are accepted;751us is a miss");
        feed(p,2,1100000,1100000,int64_t(B+excess),true,995000,200000000,0,0,10000,T);
        stats=p.stats();
        check(excess>T ? stats.currentUs==B+1 : stats.currentUs<=B,
            "tolerance-covered input cannot grow delay; one excess microsecond requests only one microsecond");
        check((stats.raises>0)==(excess>T),"growth decisions agree with tolerance-aware causal coverage");
        if(excess==690) {
            feed(p,3,1200000,1200000,B+excess,true,995000,200000000,0,0,10000,T);
            check(p.stats().currentUs==4950,"tolerance-covered input releases25us in each100ms without growth");
            feed(p,4,1300000,1300000,B+excess,true,995000,200000000,0,0,10000,T);
            check(p.stats().currentUs==4940 && p.stats().raises==0 && p.stats().missed==0,
                "tolerance-aware demand reaches4940us afterthree100ms observations with no miss or upward reversal");
        }
    }
    InputPercentileBuffer zero;zero.reset(B);
    feed(zero,1,1000000,1000000,B+690,true,995000,200000000,0,0,10000,0);
    check(zero.stats().targetUs==B+690 && zero.stats().missed==1,
        "zero tolerance retains the old exact percentile and miss semantics");

    InputPercentileBuffer p;p.reset(B);
    feed(p,1,1000000,1000000,5690,true,995000,200000000,200,0,10000,T);
    check(p.stats().rawRequiredQuantileUs==5890 && p.stats().targetUs==4940,
        "budget normalization precedes the single tolerance subtraction");
    p.advance(1100000,0,10000,995000,200000000,1000,250,600,T);
    check(p.stats().samples==1 && p.stats().rawRequiredQuantileUs==5890 && p.stats().targetUs==4540,
        "rising readiness budget renormalizes retained raw input without double tolerance");
    p.advance(1200000,0,10000,995000,200000000,1000,250,-500,T);
    check(p.stats().targetUs==5640,
        "negative readiness budget remains signed before tolerance-aware buffer demand");
    p.advance(1300000,0,10000,995000,200000000,1000,250,-500,0);
    check(p.stats().samples==1 && p.stats().targetUs==6390 && p.stats().toleranceUs==0,
        "runtime tolerance decrease updates retained quantile demand without requiring a new input");
    p.advance(1400000,0,10000,995000,200000000,1000,250,200,2000);
    check(p.stats().samples==1 && p.stats().targetUs==3690 && p.stats().recentReleaseMisses==0,
        "runtime tolerance increase refreshes target and current-CDF miss classification");

    InputPercentileBuffer history;history.reset(B);
    feed(history,1,1000000,1000000,B+690,true,995000,200000000,0,0,10000,0);
    history.advance(1100000,0,10000,995000,200000000,1000,250,0,T);
    check(history.stats().samples==1 && history.stats().targetUs==B-60 && history.stats().recentReleaseMisses==0 &&
        history.stats().recentMissed==1 && history.stats().cumulativeMissed==1,
        "dynamic tolerance reinterprets retained CDF while historical outcomes preserve observed per-frame definitions");

    p.reset(B);feed(p,1,2000000,2000000,-600,true,995000,200000000,-200,0,10000,T);
    check(p.stats().rawRequiredQuantileUs==-800 && p.stats().targetUs==0 && p.stats().missed==0,
        "negative causal input remains early and cannot become an unsigned tolerance miss");
    p.reset(B);feed(p,1,3000000,3000000,10000,true,995000,200000000,0,0,6000,T);
    check(p.stats().targetUs==6000 && p.stats().clipped && p.stats().headroomTargetUnclippedUs==9250,
        "tolerance subtraction precedes maximum-delay clipping");
    p.reset(B);feed(p,1,4000000,4000000,std::numeric_limits<int64_t>::max(),true,995000,200000000,0,0,std::numeric_limits<uint64_t>::max(),T);
    check(p.stats().targetUs==uint64_t(std::numeric_limits<int64_t>::max())-T,
        "large signed positive input subtracts tolerance without overflow");
    p.reset(std::numeric_limits<uint64_t>::max());
    feed(p,1,5000000,5000000,std::numeric_limits<int64_t>::max(),true,995000,200000000,0,0,std::numeric_limits<uint64_t>::max(),std::numeric_limits<uint64_t>::max());
    check(p.stats().targetUs==0 && p.stats().missed==0,
        "maximum tolerance and buffer do not wrap the accepted boundary into a miss");
}
void testStartupLearning() {
    InputPercentileBuffer p;p.reset(0);
    check(p.learning(),"a fresh buffer starts learning");
    feed(p,1,1000000,1000000,9000,false);
    check(p.demand(0)==0,"learning never grows from ineligible input");
    p.advance(1050000);
    check(p.demand(0)==0,"learning never grows at decision time");
    feed(p,2,1100000,1100000,2000);
    check(p.demand(0)==2000,"before the tail is supported, protection follows the largest eligible requirement at once");
    feed(p,3,1110000,1110000,3500);
    check(p.demand(0)==3500 && p.learning(),"a larger requirement while learning is protected at once");
    for(uint64_t i=4;i<=101;++i) feed(p,i,1110000+i*1000,1110000+i*1000,500);
    check(!p.learning() && !p.stats().tailUncertain,"a supported profile tail ends learning");
    const auto held=p.demand(0);
    feed(p,102,1300000,1300000,6000);feed(p,103,1400000,1400000,6000);
    check(p.demand(0)>held && p.demand(0)<=held+101,"after learning, growth is slewed at 1000 us per second");
    p.breakSequence();
    check(p.learning(),"a source rebase discards evidence and relearns");
}
void testSlewBoundsAndReset() {
    InputPercentileBuffer p;p.reset(0);p.endLearning();
    feed(p,1,1000000,1000000,2000);feed(p,2,1100000,1100000,2000);
    check(p.demand(0)==100,"experimental upward slew is 1000 us per second");
    p.reset(1000);feed(p,1,1000000,1000000,0);feed(p,2,1100000,1100000,0);
    check(p.demand(0)==975,"downward slew is 250 us per second");
    auto before=p.demand(0);p.breakSequence();
    check(p.demand(0)==before && p.stats().samples==0,"source epoch clears evidence while retaining applied delay");
    feed(p,1,2000000,1000000,9000,true,990000,30000000,0,500,800);
    check(p.demand(0)>=500 && p.demand(0)<=800 && p.stats().targetUs==800 && p.stats().clipped,"changing dynamic bounds clamp current and target");
    feed(p,2,1999999,1008333,100);
    check(p.stats().samples==0,"nonmonotonic observation clock invalidates evidence");
}
void testInvalidCapacityAndExpiry() {
    InputPercentileBuffer p;p.reset(6000);
    feed(p,1,1000000,1000000,5000);
    feed(p,2,1100000,1010000,25000,false);
    check(p.stats().samples==1 && p.stats().evidenceValid && p.stats().eligible==1 && p.stats().targetUs==5000,"unabsorbable service cannot add percentile pressure");
    feed(p,3,1200000,1020000,5000);
    check(p.stats().validPairCoverageUs==0,"invalid eligibility cannot bridge input pair coverage");
    // Separate high-delay fixture: prolonged unknown input must not pin delay.
    p.reset(6000);
    for(uint64_t i=1;i<=601;++i) feed(p,i,i*100000,i*8333,25000,false);
    check(p.stats().expiredUnknown && !p.stats().evidenceValid && p.demand(0)==0,"expired capacity evidence recovers to floor without claiming clean playback");
    p.reset(6000);
    for(uint64_t i=1;i<=601;++i) {
        if(i%10==0) p.breakSequence();
        feed(p,i,i*100000,i*8333,25000,false);
    }
    check(p.demand(0)==0 && p.stats().epochResets==60 && p.stats().unknownRelease>0,
        "repeated invalid source epochs do not rearm unknown evidence retention indefinitely");
    p.reset(1600);feed(p,1,1000000,1000000,1000);feed(p,3,1100000,1020000,1000);
    check(p.stats().validPairCoverageUs==0,"missing frame identity does not bridge qualified pairs");
}
void testHistoryAgingWithoutEligibleInput() {
    InputPercentileBuffer p;p.reset(1000);
    feed(p,1,1000000,1000000,2000,true,990000,10000000);
    feed(p,2,2000000,1010000,100,true,990000,10000000);
    check(p.stats().recentEligible==2 && p.stats().recentMissed==1 && p.stats().recentAbsorbed==1,
        "recent input outcomes cover eligible observations in the active history");
    feed(p,3,11000000,1020000,9000,false,990000,10000000);
    auto stats=p.stats();
    check(stats.samples==1 && stats.rawRequiredQuantileUs==100 && stats.targetUs==100,
        "oldest high input expires at exact window boundary during invalid observation while retained low input drives target");
    check(stats.recentEligible==1 && stats.recentMissed==0 && stats.recentAbsorbed==1,
        "recent absorbed and missed counts age with samples even during invalid capacity");
    feed(p,4,12000000,1030000,9000,false,990000,10000000);
    stats=p.stats();
    check(stats.samples==0 && stats.rawRequiredQuantileUs==0 && stats.headroomTargetUnclippedUs==0 &&
        stats.observationSpanUs==0 && stats.validPairCoverageUs==0,
        "empty expired history clears stale quantile, coverage and headroom diagnostics");
    check(stats.recentEligible==0 && stats.recentAbsorbed==0 && stats.recentMissed==0 && stats.absorbed==1 && stats.missed==1,
        "recent counters expire while cumulative absorbed and missed counters remain explicit");
    p.breakSequence();check(p.stats().recentEligible==0 && p.stats().absorbed==1 && p.stats().missed==1,
        "basis break clears recent evidence without erasing cumulative outcomes");
    p.reset();check(p.stats().absorbed==0 && p.stats().missed==0,
        "full reset clears cumulative and recent input outcome history");

    p.reset(1000);
    feed(p,1,1000000,1000000,2000,true,990000,30000000);
    feed(p,2,2000000,1010000,100,true,990000,30000000);
    const auto observed=p.stats().observed;
    p.advance(11000000,0,6000,990000,30000000,1000,250);
    check(p.stats().samples==2 && p.stats().targetUs==2000 && p.stats().observed==observed,
        "idle decision advance ages and slews without inventing a newly observed frame");
    p.advance(11000001,0,6000,990000,10000000,1000,250);
    check(p.stats().samples==1 && p.stats().targetUs==100,
        "shorter profile history takes effect immediately during idle decision advance");
    p.advance(11000002,0,6000,990000,30000000,1000,250);
    check(p.stats().samples==1 && p.stats().targetUs==100,
        "longer profile history cannot resurrect already expired high input");
    p.advance(32000000);
    check(p.stats().samples==0 && p.stats().observed==observed,
        "idle advance expires all input without changing observed or eligible outcome counts");
    feed(p,3,32000000,1020000,300,true,990000,30000000);
    check(p.stats().observed==observed+1 && p.stats().samples==1,
        "same-time observation after decision advance is accepted without clock reset");

    p.reset(1000);
    for(uint64_t i=1;i<=100;++i) feed(p,i,1000000+i*10000,1000000+i*10000,i==1?2000:100,true,990000,30000000);
    check(p.stats().targetUs==100,"supported p99 sample mixture starts at lower quantile");
    feed(p,101,2100000,2010000,9000,false,999900,30000000);
    check(p.stats().targetUs==2000 && p.stats().tailUncertain && p.stats().samples==100,
        "strict profile probability applies to retained history even with no new eligible input");
}
void testNoRunawayAndQualifiedGrowth() {
    InputPercentileBuffer p;p.reset(0);p.endLearning();
    feed(p,0,1000000,1000000,4000,true,990000,10000000);
    p.advance(1100000);
    check(p.stats().targetUs==4000 && p.demand(0)==0 && p.stats().growthBlocked,"decision advance cannot raise buffer from retained high input requirements");
    feed(p,1,1200000,1010000,9000,false,990000,10000000);
    check(p.demand(0)==0 && p.stats().samples==1 && p.stats().growthBlocked &&
        !p.stats().lastInputEligible && p.stats().lastInputRequiredUs==9000 && p.stats().lastInputObservationUs==1200000,"invalid overloaded input cannot raise buffer from historical high target");
    feed(p,2,1300000,1020000,4000,true,990000,10000000);
    check(p.demand(0)==100 && !p.stats().growthBlocked && p.stats().lastInputEligible,"qualified observation grows only for its bounded observation interval, without banked invalid time");
    p.reset(0);p.endLearning();feed(p,0,1000000,1000000,4000,true,990000,10000000);
    for(uint64_t i=1;i<=120;++i) {
        const uint64_t at=1000000+i*8333;
        p.advance(at);feed(p,i,at,at,4000,true,990000,10000000);
    }
    check(p.demand(0)==999,"interleaved decision advances preserve fractional 1000 us per second eligible growth");
    const auto before=p.demand(0);
    feed(p,121,1000000+121*8333,1000000+121*8333,9000,false,990000,10000000);
    feed(p,122,1000000+122*8333,1000000+122*8333,4000,true,990000,10000000);
    check(p.demand(0)==before+8,"invalid observation clears partial positive credit rather than banking a later burst");

    p.reset(1000);
    for(uint64_t i=1;i<=100;++i) feed(p,i,1000000+i*10000,1000000+i*10000,1000,true,990000,10000000);
    check(p.stats().targetUs==p.stats().currentUs,"qualified input fixture begins at direct hold equilibrium");
    for(uint64_t i=101;i<=300;++i) feed(p,i,1000000+i*10000,1000000+i*10000,3000,true,990000,10000000);
    check(p.stats().targetUs==3000 && p.demand(0)>1000 && p.stats().raises>0,
        "genuine repeated eligible input headroom violations grow from hold without legacy quality-debt hold blocking attack");
    for(uint64_t i=301;i<=4000;++i) feed(p,i,1000000+i*10000,1000000+i*10000,9000,true,990000,10000000);
    check(p.demand(0)==6000 && p.stats().targetUs==6000 && p.stats().clipped,
        "sustained unbounded input tail saturates at dynamic maximum without runaway delay");
    for(uint64_t i=4001;i<=8000;++i) feed(p,i,1000000+i*10000,1000000+i*10000,0,true,990000,10000000);
    check(p.demand(0)==0 && p.stats().targetUs==0,
        "saturated input protection ages out and recovers after a genuine clean burst transition");

    p.reset(1500);feed(p,1,1000000,500000000,500,true,990000,10000000,200);
    const auto raw=p.stats().rawRequiredQuantileUs;
    p.breakSequence();feed(p,2,1100000,900000000,100,true,990000,10000000,600);
    check(p.stats().rawRequiredQuantileUs==raw && p.stats().targetUs==100,
        "own readiness budget and source epoch rebase do not manufacture new raw input headroom");
}
void testUnthrottledInputPercentileRelease() {
    InputPercentileBuffer p;p.reset(6000);
    for(uint64_t i=1;i<=400;++i) {const auto at=1000000+i*1000000/60;
        p.observe(i,at,at,i==400?7000:0,true,0,10000,995000,200000000,1000,0);}
    const uint64_t end=1000000+400ULL*1000000/60;
    p.noteBufferMiss(end,true,true);p.advance(end,0,10000,995000,200000000,1000,250);
    check(p.stats().targetUs==0 && p.stats().recentReleaseMisses==1 && p.stats().releaseFactorPpm==1000000 &&
        p.stats().effectiveFallRateUsPerSecond==250 && p.stats().holdUntilUs==0,
        "trajectory experiment retains long quantile target and diagnostics while disabling both hard pause and soft fall heuristic");
    for(uint64_t elapsed=100000;elapsed<=24000000;elapsed+=100000) p.advance(end+elapsed);
    check(p.demand(0)==0,"6000us protection releases to floor in24s at unchanged250us/s without short heuristic");
}
void testProbabilitySupportAndCap() {
    InputPercentileBuffer p;p.reset(0);
    for(uint64_t i=1;i<=199;++i) feed(p,i,i*10000,i*10000,i==1?2000:100,true,995000);
    check(p.stats().tailUncertain,"99.5 percent probability needs two hundred samples");
    feed(p,200,2000000,2000000,100,true,995000);
    check(!p.stats().tailUncertain && p.stats().targetUs==100,"99.5 percent support threshold and nearest rank are exact");
    p.reset(0);
    for(uint64_t i=1;i<=3600;++i) feed(p,i,i*8333,i*8333,100,true,999900);
    check(p.stats().tailUncertain,"99.99 percent tail remains uncertain at 120 Hz in thirty seconds");
    p.reset(0);
    for(uint64_t i=1;i<=InputPercentileBuffer::MaximumSamples+1;++i) feed(p,i,i*1000,i*1000,100,true,990000,300000000);
    check(p.stats().samples==InputPercentileBuffer::MaximumSamples && p.stats().historyTruncated,"hard sample cap bounds malformed rates");
    feed(p,InputPercentileBuffer::MaximumSamples+2,262200000,262200000,100,true,990000,300000000);
    check(p.stats().tailUncertain,"history truncation remains explicit in target uncertainty");
}
#ifdef INPUT_PERCENTILE_CONTROLLER_TEST
void testFactoryProfileOwnsAdaptiveTarget() {
    for(int mode:{0,1,2}) {
        for(int custom:{0,9900,9950,9999,-1,12,10000,999999}) {
            VrrSessionConfig session;session.streamRateHz=120;session.displayRefreshHz=144;
            session.latencyMode=mode;session.timingOptions.targetHundredths=custom;
            session.timingOptions.historySeconds=custom==0?0:200;
            session.experimentalInputPercentileWindowUs=30000000;
            const auto expected=uint64_t(session.timingOptions.resolved(mode).targetHundredths)*100;
            const auto disabled=vrrTimingParametersForSession(session);
            check(disabled.playoutInputPercentileToleranceUs==0,"disabled adaptive mode retains serialized zero-tolerance compatibility");
            check(!disabled.playoutInputPercentileEnabled && disabled.playoutOnTimeTargetPerMillion==expected,
                "preset/custom resolution preserves ordinary severity policy when adaptive mode is disabled");
            session.experimentalInputPercentilePpm=999900;
            const auto enabled=vrrTimingParametersForSession(session);
            check(enabled.playoutInputPercentileEnabled && enabled.playoutInputPercentileTargetPerMillion==expected,
                "adaptive mode uses effective preset/custom target including lower and upper clamps");
            check(enabled.playoutInputPercentileToleranceUs==uint64_t(session.timingOptions.resolved(mode).toleranceUs),
                "adaptive input percentile uses the effective session tolerance without rewriting profile selection");
            check(enabled.playoutInputPercentileWindowUs==uint64_t(session.timingOptions.resolved(mode).historySeconds)*1000000,
                "adaptive history uses the effective preset/custom profile rather than an environment history override");
            check(enabled.playoutOnTimeTargetPerMillion==expected,
                "adaptive activation preserves selected target in interval severity diagnostics");
        }
    }
    VrrSessionConfig user;user.experimentalInputPercentilePpm=1;user.timingOptions.toleranceUs=750;
    user.timingOptions.targetHundredths=9950;user.timingOptions.historySeconds=200;
    const auto userPolicy=vrrTimingParametersForSession(user);
    check(userPolicy.playoutInputPercentileToleranceUs==750 && userPolicy.playoutInputPercentileTargetPerMillion==995000 &&
        userPolicy.playoutInputPercentileWindowUs==200000000,"actual750us user profile sizes the adaptive quantile without changing99.50percent/200s");
    for(int history:{0,1,10,200,300,999}) {
        VrrSessionConfig session;session.latencyMode=1;session.experimentalInputPercentilePpm=1;
        session.timingOptions.historySeconds=history;session.experimentalInputPercentileWindowUs=30000000;
        const auto policy=vrrTimingParametersForSession(session);
        check(policy.playoutInputPercentileWindowUs==uint64_t(session.timingOptions.resolved(1).historySeconds)*1000000,
            "adaptive profile history includes exact supported bounds and lower/upper clamping");
    }
}
void testActualControllerRateBandsRestoreProtection() {
    // The user's round trip: 120 FPS needs buffer, 30 FPS needs none (and has
    // less lateness, as in the 22:55 capture), then back to 120 FPS.
    VrrSessionConfig session;session.streamRateHz=120;session.displayRefreshHz=144;
    session.smoothFrameTiming=false;session.experimentalInputPercentilePpm=995000;
    session.timingOptions={1000,9950,60,500};
    auto policy=vrrTimingParametersForSession(session);
    policy.playoutInputHitchMissesOnly=6;policy.playoutInputStableAdaptation=0; // Preserve the recorded-policy regression fixture.
    // Exercise the explicit 12 ms fixture budget; the independent low-latency
    // half-refresh cap would make its 5 ms spikes unpreventable by definition.
    policy.latencyFixEnabled=0;
    policy.playoutDelayStartSeedUs=1000;policy.playoutDelayMinimumUs=1000;policy.playoutDelayMaximumUs=12000;
    policy.playoutDelayMaximumPeriodPerMille=0;policy.playoutDelayCapSourcePeriodPerMille=0;
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
    check(slow.recentEligible>0 && slow.recentEligible<=600 && slow.recentMissed==0,
          "the 30 FPS layer's coverage counts only its own frames");
    check(back.currentUs+250>=fast.currentUs,"returning to 120 FPS restores that band's protection within a few frames");
    check(eligibleBack>400 && missedBack*200<=eligibleBack,
          "frames after the return are covered at the profile target, not relearned through misses");
    check(settled.recentEligible==fast.recentEligible+eligibleBack &&
          settled.recentMissed==fast.recentMissed+missedBack,
          "return coverage preserves that band's actual history, including genuine new misses, without inventing target attainment");
}

void testOrdinaryVariationKeepsLayer() {
    for (const uint64_t version : {7,8,9}) {
        VrrSessionConfig session;session.streamRateHz=116;session.displayRefreshHz=120;
        session.experimentalInputPercentilePpm=1;session.timingOptions={1000,9950,120,500};
        auto policy=vrrTimingParametersForSession(session);
        policy.playoutInputHitchMissesOnly=version;policy.playoutInputStableAdaptation=version>=10;policy.latencyFixEnabled=0;
        VrrTimingController controller(session,true,policy);
        uint64_t slot=1000000,submitted=0;uint32_t pts=0;int frame=0;
        const auto feed=[&](uint32_t ticks) {
            slot+=uint64_t(ticks)*1000000/90000;pts+=ticks;
            const uint64_t input=slot+1000;
            PacedFrame f(nullptr,++frame,pts,true,input);
            f.setDeliveryTimeline(input-800,input-600,input-500);
            const auto d=controller.schedule(f,std::max(input,submitted));
            const auto ready=std::max(input,d.renderStartUs)+500;
            submitted=std::max(ready,d.targetUs);
            controller.notePreparationDuration(500,0,ready);
            controller.noteSchedulerDelays(0,0,true);
            controller.noteSubmission(true,false,submitted,submitted);
        };
        for(int i=0;i<600;++i) feed(990); // settled ~91 FPS, startup anchor at 116
        const auto anchor=controller.inputLayerPeriodUs();
        const auto before=controller.intervalStats().inputPercentile;
        check(before.recentEligible>400,"ordinary variation regression has established coverage");
        // RTP intervals around the reported first false switch, frames 567-569.
        for (auto ticks : {1138u,1198u,1224u}) feed(ticks);
        if(version==7) {
            check(controller.inputLayerPeriodUs()!=anchor &&
                  controller.intervalStats().inputPercentile.recentEligible==0,
                  "regression reproduces old false layer selection and vanished coverage");
            continue;
        }
        check(controller.inputLayerPeriodUs()==anchor &&
              controller.intervalStats().inputPercentile.recentEligible>=before.recentEligible,
              "brief source tails keep both layer and displayed history");
        // The long cadence fit also crosses the old anchor's boundary. This
        // must not bypass qualification while actual output remains variable.
        for(int i=0;i<300;++i) {
            feed(i%2?1440:1170); // alternating 16/13 ms, ~69 FPS
            check(controller.inputLayerPeriodUs()==anchor,
                  "the moving source fit cannot switch layers by itself");
        }
        const auto prior=controller.intervalStats().inputPercentile;
        for(int i=0;i<3;++i) feed(3000);
        check(controller.inputLayerPeriodUs()>=33332 && controller.inputLayerPeriodUs()<=33334,
              "a subsequent large step still switches promptly after ordinary variation");
        const auto slow=controller.intervalStats().inputPercentile;
        check(slow.recentEligible>=prior.recentEligible && slow.recentMissed>=prior.recentMissed,
              "a legitimate layer transition cannot erase observed coverage or misses");
        controller.rebase();
        const auto rebased=controller.intervalStats().inputPercentile;
        check(rebased.recentEligible==slow.recentEligible && rebased.recentMissed==slow.recentMissed,
              "a source rebase invalidates learning coordinates without erasing actual outcomes");
        controller.reset();
        check(controller.intervalStats().inputPercentile.recentEligible==0,
              "a new session starts a fresh reported coverage history");
    }
}

void testFastControllerLayerRoundTrip(uint64_t version, bool bounded=false, uint64_t stableRevision=1) {
    VrrSessionConfig session;session.streamRateHz=120;session.displayRefreshHz=120;
    session.latencyMode=1;session.smoothFrameTiming=false;session.experimentalInputPercentilePpm=1;
    session.experimentalBoundedCatchup=bounded;
    session.timingOptions={1000,9950,120,500};
    auto policy=vrrTimingParametersForSession(session);
    policy.latencyFixEnabled=0;
    policy.playoutInputHitchMissesOnly=version;policy.playoutInputStableAdaptation=version>=10?stableRevision:0;
    policy.playoutDelayStartSeedUs=4000;policy.playoutDelayMinimumUs=1000;
    policy.playoutDelayMaximumUs=12000;
    policy.playoutDelayMaximumPeriodPerMille=policy.playoutDelayCapSourcePeriodPerMille=0;
    VrrTimingController controller(session,true,policy);
    uint64_t slot=1000000,submitted=0;uint32_t pts=0;int frame=0;
    VrrTimingDecision last;
    const auto feed=[&](int count,uint64_t period,uint32_t ticks,uint64_t spike) {
        for(int n=0;n<count;++n,++frame) {
            slot+=period;pts+=ticks;
            const uint64_t input=slot+1000+(frame%20==0?spike:0);
            PacedFrame f(nullptr,frame,pts,true,input);
            f.setDeliveryTimeline(input-800,input-600,input-500);
            last=controller.schedule(f,std::max(input,submitted));
            const uint64_t ready=std::max(input,last.renderStartUs)+500;
            submitted=std::max(ready,last.targetUs);
            controller.notePreparationDuration(500,0,ready);
            controller.noteSchedulerDelays(0,0,true);
            controller.noteSubmission(true,false,submitted,submitted);
        }
    };
    feed(1200,11111,1000,4000);
    const auto fast=controller.intervalStats().inputPercentile;
    check(fast.recentEligible>500 && fast.currentUs>1500,
          "fast layer round trip starts with qualified learned high-rate protection");
    feed(3,33333,3000,1000);
    if(version>=9)
        check(controller.intervalStats().inputPercentile.currentUs+500>=fast.currentUs,
              "the live controller selects 30 FPS without instantly removing the old buffer");
    check(last.sourcePeriodUs>=33332 && last.sourcePeriodUs<=33334 &&
          (version<8 || controller.inputLayerPeriodUs()>=33332) &&
          (version<8 ? controller.intervalStats().inputPercentile.recentEligible<fast.recentEligible :
           controller.intervalStats().inputPercentile.recentEligible>=fast.recentEligible),
          "three 30 FPS source intervals immediately select a separate layer");
    feed(180,33333,3000,1000);
    const auto slow=controller.intervalStats().inputPercentile;
    feed(3,11111,1000,4000);
    const auto back=controller.intervalStats().inputPercentile;
    check(last.sourcePeriodUs>=11110 && last.sourcePeriodUs<=11112 &&
          back.recentEligible>=fast.recentEligible && back.currentUs+250>=fast.currentUs,
          "three returning 90 FPS intervals restore the saved queue and history without relearning");
    check(back.missed-slow.missed<=1,"fast return does not require a run of new input misses");
    feed(3,33333,3000,1000);
    feed(3,10000,900,4000);
    const auto shortReturn=controller.intervalStats().inputPercentile;
    check(last.sourcePeriodUs==10000 && shortReturn.recentEligible>=fast.recentEligible &&
          shortReturn.currentUs+500>=fast.currentUs,
          "a short 30 FPS visit returning to 100 FPS preserves the saved high-rate protection");
    const uint64_t priorEligible=shortReturn.recentEligible;
    for(int i=0;i<40;++i) feed(3,i%2?11111:8182,i%2?1000:736,0);
    check(controller.intervalStats().inputPercentile.recentEligible>=priorEligible,
          "ordinary 90-110 FPS variation keeps the active layer's evidence");
}

void testRateBaselineStepIsNotClockDrift() {
    const auto run=[](uint64_t version,int64_t driftPpm=0) {
        VrrSessionConfig session;session.streamRateHz=90;session.displayRefreshHz=120;
        session.experimentalBoundedCatchup=true;
        session.experimentalInputPercentilePpm=995000;session.timingOptions={1000,9950,120,500};
        auto policy=vrrTimingParametersForSession(session);
        // Compare the historical producer-clock policies explicitly. New live
        // sessions use the independently tested packet-receive clock (mode 4).
        policy.playoutInputClockRelativeHistory=3;
        policy.playoutInputHitchMissesOnly=version;policy.playoutInputStableAdaptation=version>=10;policy.latencyFixEnabled=0;
        policy.playoutDelayStartSeedUs=1000;
        VrrTimingController c(session,true,policy);
        uint64_t slot=1000000,submitted=0,pts=0;int frame=0;
        int64_t historyBefore=0,mappingBefore=0;
        const auto feed=[&](int count,uint64_t period,uint32_t step,uint64_t base) {
            for(int n=0;n<count;++n,++frame) {
                slot+=period;pts+=step;
                const int64_t drift=(int64_t(slot)-1000000)*driftPpm/1000000;
                const uint64_t decoded=uint64_t(int64_t(slot+base)+drift);
                PacedFrame f(nullptr,frame,uint32_t(pts),true,decoded);
                f.setDeliveryTimeline(decoded-800,decoded-600,decoded-500);
                const auto d=c.schedule(f,std::max(decoded,submitted));
                const uint64_t ready=std::max(decoded,d.renderStartUs)+500;
                submitted=std::max(ready,d.targetUs);
                c.notePreparationDuration(500,0,ready);c.noteSchedulerDelays(0,0,true);
                c.noteSubmission(true,false,submitted,submitted);
            }
        };
        feed(600,33333,3000,1000);
        feed(2700,11111,1000,9000); // Same clock; rate-dependent producer baseline +8 ms.
        historyBefore=c.intervalStats().inputPercentile.clockOffsetUs;
        mappingBefore=c.playoutOffsetUs();
        feed(1350,11111,1000,9000);
        const int64_t historyChange=c.intervalStats().inputPercentile.clockOffsetUs-historyBefore;
        const int64_t mappingChange=c.playoutOffsetUs()-mappingBefore;
        std::fprintf(stderr,"rate baseline policy %llu: history change %lld, mapping change %lld us\n",
            (unsigned long long)version,(long long)historyChange,(long long)mappingChange);
        return historyChange;
    };
    const auto historical=run(2),corrected=run(6);
    check(std::abs(historical)>1000,"fixture: historical drift fit mistakes a rate-dependent baseline for clock skew");
    check(std::abs(corrected)<250,"settled history does not keep inventing drift from the preceding rate transition");
    check(std::abs(run(6,50))<250 && std::abs(run(6,-50))<250,
          "after the rate baseline settles, genuine clock drift is learned again in both directions");
}
void testActualControllerInputMissReleaseFeedbackApi() {
    VrrSessionConfig session;session.streamRateHz=60;session.displayRefreshHz=120;
    VrrTimingController ordinary(session);
    ordinary.noteInputBufferMiss(1000000,true,true);
    check(ordinary.intervalStats().inputPercentile.cumulativeNetworkMisses==0 &&
        ordinary.intervalStats().inputPercentile.cumulativeDecoderMisses==0,
        "input buffer-line pause API has no effect on ordinary production mode");
    session.experimentalInputPercentilePpm=1;session.timingOptions.targetHundredths=9950;
    session.timingOptions.historySeconds=200;
    VrrTimingController experimental(session,true,vrrTimingParametersForSession(session));
    experimental.noteInputBufferMiss(1000000,true,false);
    auto stats=experimental.intervalStats().inputPercentile;
    check(stats.holdUntilUs==0 && stats.holdRemainingUs==0 && stats.cumulativeNetworkMisses==1 && stats.cumulativeDecoderMisses==0,
        "actual opt-in controller records network buffer-line diagnostics without a hard pause");
    experimental.noteInputBufferMiss(6000000,false,true);
    stats=experimental.intervalStats().inputPercentile;
    check(stats.holdUntilUs==0 && stats.holdRemainingUs==0 && stats.cumulativeNetworkMisses==1 && stats.cumulativeDecoderMisses==1,
        "actual opt-in controller independently records decode buffer-line diagnostics");
    experimental.noteInputBufferMiss(7000000,false,false);
    check(experimental.intervalStats().inputPercentile.holdUntilUs==0,
        "actual controller submission/presentation-only errors cannot arm a hard pause");
    experimental.rebase();
    check(experimental.intervalStats().inputPercentile.holdUntilUs==0 && experimental.intervalStats().inputPercentile.cumulativeNetworkMisses==1 && experimental.intervalStats().inputPercentile.recentReleaseSamples==0,
        "actual source rebase clears short feedback and preserves stage totals without a hard pause");
    experimental.reset();
    check(experimental.intervalStats().inputPercentile.holdUntilUs==0 && experimental.intervalStats().inputPercentile.cumulativeNetworkMisses==0,
        "actual full controller reset clears input diagnostics");
}
void testActualControllerRepeatedZeroTimestampDrops() {
    VrrSessionConfig session;session.streamRateHz=60;session.displayRefreshHz=120;
    session.smoothFrameTiming=false;session.experimentalInputPercentilePpm=990000;
    session.experimentalInputPercentileWindowUs=30000000;
    session.timingOptions.historySeconds=30;
    auto policy=vrrTimingParametersForSession(session);
    policy.playoutDelayStartSeedUs=6000;
    policy.playoutDelayMinimumUs=1000;policy.playoutDelayMaximumUs=6000;
    policy.playoutDelayMaximumPeriodPerMille=0;
    policy.playoutDelayCapSourcePeriodPerMille=0;
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
    check(stats.expiredUnknown && stats.currentUs==stats.minimumUs && stats.unknownRelease>0 && stats.epochResets>50,
        "repeated zero-timestamp drops and phase resets release obsolete seed after history expiry and fall slew");
}
void testActualControllerRequiresAnInputHitch() {
    for(const int seconds:{10,37,60,120,300}) {
        for(const int target:{9900,9950,9999}) {
            VrrSessionConfig settings;settings.experimentalInputPercentilePpm=1;
            settings.timingOptions={1000,target,seconds,500};
            const auto configured=vrrTimingParametersForSession(settings);
            check(configured.playoutInputStableAdaptation==5 &&
                  configured.playoutInputPercentileWindowUs==uint64_t(seconds)*1000000 &&
                  configured.playoutInputPercentileTargetPerMillion==uint64_t(target)*100,
                "production rolling estimator receives the user's custom history and smoothness settings");
        }
    }
    VrrSessionConfig session;session.streamRateHz=100;session.displayRefreshHz=240;
    session.latencyMode=1;session.smoothFrameTiming=false;session.experimentalInputPercentilePpm=1;
    session.timingOptions={1000,9950,10,500};
    auto base=vrrTimingParametersForSession(session);
    check(base.playoutInputHitchMissesOnly==10 && base.playoutInputStableAdaptation==5,"production keeps preventable-miss scoring with stable target adaptation");
    base.playoutDelayStartSeedUs=1000;base.playoutDelayMinimumUs=1000;base.playoutDelayMaximumUs=6000;
    base.playoutDelayMaximumPeriodPerMille=base.playoutDelayCapSourcePeriodPerMille=0;
    base.playoutInputLearningTrim=0;
    base.latencyFixEnabled=0;
    base.playoutCatchupPerMille=base.playoutLateRecovery=0;
    base.playoutInputBoundedRecoveryCredit=1;
    uint64_t misses[3]={},raises[3]={};
    for(int scenario=0;scenario<3;++scenario) {
        auto p=base;
        if(scenario==0) { // Recorded raw-input policy.
            p.playoutInputHitchMissesOnly=0;
            p.playoutInputStableAdaptation=0;
        }
        VrrTimingController controller(session,true,p);
        for(int i=0;i<1500;++i) {
            const uint64_t source=1000000+uint64_t(i)*10000;
            const uint64_t input=source+(i>100 && i%2 ? 3500 : 0);
            PacedFrame frame(nullptr,i,uint32_t(i*900),true,input);
            frame.setDeliveryTimeline(input,input,input);
            const auto decision=controller.schedule(frame,input);
            const uint64_t ready=std::max(input,decision.renderStartUs)+500;
            // Constant actual cadence despite late input, versus a real
            // readiness-induced stretch/compression pair in scenario 2.
            const uint64_t submitted=scenario==2 ? std::max(ready,decision.targetUs) : source+8000;
            controller.notePreparationDuration(500,0,ready);
            controller.noteSchedulerDelays(0,0,true);
            controller.noteSubmission(true,false,submitted,submitted);

        }
        const auto stats=controller.intervalStats().inputPercentile;
        misses[scenario]=stats.missed;raises[scenario]=stats.raises;
        std::fprintf(stderr,"hitch fixture scenario %d eligible %llu misses %llu raises %llu current %llu target %llu required %lld\n",
            scenario,(unsigned long long)stats.eligible,(unsigned long long)stats.missed,
            (unsigned long long)stats.raises,(unsigned long long)stats.currentUs,
            (unsigned long long)stats.targetUs,(long long)stats.lastInputRequiredUs);
        check(stats.eligible>1000,"hitch provenance fixture must retain qualified input evidence");
        if(scenario==1) check(stats.lastInputRequiredUs>1500 && stats.currentUs==1000,
            "actual clean spacing can coexist with input beyond B+T; it must not buy latency");
    }
    check(misses[0]>0 && raises[0]>0,"recorded policy must reproduce raw-lateness scoring and growth on clean spacing");
    check(misses[1]==0 && raises[1]==0,"late input without an actual hitch must not affect the goal or grow the buffer");
    check(misses[2]>0 && misses[2]<=150 && raises[2]>0,
        "the same input spikes must acquire enough protection to limit repeated preventable hitches");
}

void testActualControllerPostInputErrorsCannotInflateBuffer() {
    VrrSessionConfig session;session.streamRateHz=60;session.displayRefreshHz=120;
    session.latencyMode=1;session.smoothFrameTiming=false;session.experimentalInputPercentilePpm=1;
    session.timingOptions={1000,9950,10,500};
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
    check(b.inputPercentile.rawRequiredQuantileUs==a.inputPercentile.rawRequiredQuantileUs,
        "own buffering and rebases preserve raw causal input requirement under downstream errors");
}
void testActualControllerOptInAndEligibility() {
    VrrSessionConfig baseline;baseline.streamRateHz=120;baseline.displayRefreshHz=144;
    baseline.smoothFrameTiming=false;
    VrrTimingController production(baseline);
    check(!production.intervalStats().inputPercentileEnabled,"default production controller keeps percentile mode disabled");
    auto session=baseline;session.experimentalInputPercentilePpm=995000;
    session.experimentalInputPercentileWindowUs=60000000;
    auto policy=vrrTimingParametersForSession(session);
    check(policy.playoutInputPercentileEnabled && policy.playoutInputPercentileTargetPerMillion==
        uint64_t(session.timingOptions.resolved(session.latencyMode).targetHundredths)*100,
        "explicit activation resolves the selected profile target instead of an activation marker probability");
    check(policy.playoutOnTimeTargetPerMillion==vrrTimingParametersForSession(baseline).playoutOnTimeTargetPerMillion,
        "percentile opt-in does not rewrite severity quality target");
    auto hitchSession=session;hitchSession.readinessHitchFeedback=true;
    const auto hitchPolicy=vrrTimingParametersForSession(hitchSession);
    auto hitchBaseline=baseline;hitchBaseline.readinessHitchFeedback=true;
    check(hitchPolicy.playoutResponsiveBuffer==12 && hitchPolicy.playoutInputPercentileEnabled,
        "explicit percentile mode selects qualified interval service path with readiness hitch feedback");
    check(hitchPolicy.playoutOnTimeTargetPerMillion==vrrTimingParametersForSession(hitchBaseline).playoutOnTimeTargetPerMillion,
        "readiness hitch opt-in preserves the existing severity target");
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

}
void testExactWindowBulkExpiry() {
    InputPercentileBuffer buffer;
    buffer.reset(6000);
    buffer.setStableAdaptation(true);
    for (uint64_t frame = 1; frame <= 2000; ++frame) {
        const auto at = 1000000 + frame * 1000;
        buffer.observe(frame, at, at, 2000 + frame % 31, true,
            1000, 12000, 995000, 10000000, 1000, 250, 0, 500, 0, 0, false, 10);
    }
    auto seeded = buffer;
    seeded.markEvidenceAsSeed();
    buffer.advance(12000000);
    seeded.advance(12000000);
    const auto partial = buffer.stats();
    std::vector<int64_t> retained;
    for (uint64_t frame = 1001; frame <= 2000; ++frame)
        retained.push_back(2000 + frame % 31);
    std::sort(retained.begin(), retained.end());
    check(partial.samples == 1000 && partial.recentEligible == 1000 &&
          partial.rawRequiredQuantileUs == retained[994] && partial.validPairCoverageUs == 999000,
          "bulk expiry preserves exact nearest rank, duplicate counts, boundary and pair coverage");
    check(seeded.stats().samples == 1000 && seeded.stats().recentEligible == 0,
          "bulk expiry retains seed evidence without inventing eligible outcomes");
    buffer.advance(11999999);
    check(buffer.stats().samples == partial.samples && buffer.stats().currentUs == partial.currentUs,
          "out-of-order advances cannot retire more evidence or spend release time");
    buffer.advance(13000000);
    check(buffer.stats().samples == 0 && buffer.stats().eligible == 2000 &&
          buffer.stats().recentEligible == 0 && buffer.stats().targetUs == 1000,
          "full expiry clears indexes and window outcomes while retaining session totals");
}

int main() {
    testExactWindowBulkExpiry();
    testFastLayerCadence();testConfirmedLayerCadence();testContinuousCoverage();testResponsiveCoverageReporting();
    testGradualLayerGrowthAndHealthyRelease();
    testFeedbackBoundedRelease();
    testAsymmetricLayerBufferMovement();
    testNormalDrainAfterLayerRework();
    testStableTargetKeepsPreventableMissScoring();
    testFeedbackValidatedStableRelease();
    testContinuousStableRelease();
    testSelectedWindowReleaseGate();
    testRequiredBufferEstimateMath();
    testRollingEstimateSettingsAndConvergence();
    testSharedExtraBufferAllowance();
    testOnlyPreventableHitchesScoreAndGrow();
    testAppliedProtectionFollowsClockCoordinate();
    testClockReleaseRespectsMissGoal();
    testLiveMappingIsSeparateFromHistoryDrift();
    testMissProtectionSurvivesQuantileTrim();testMatureMissAttackUsesOnlyMeasuredDemand();
    testVrrHeadroomAbsorbsLateness();testPhaseBreakKeepsEvidenceAndHeadroomLossRestores();testStartupLearning();
    testQuantileAndClock();testBudgetAndSignedOffsets();testInputToleranceBoundaryAndNormalization();testSlewBoundsAndReset();
    testInvalidCapacityAndExpiry();testProbabilitySupportAndCap();testHistoryAgingWithoutEligibleInput();testNoRunawayAndQualifiedGrowth();testUnthrottledInputPercentileRelease();
#ifdef INPUT_PERCENTILE_CONTROLLER_TEST
    testFactoryProfileOwnsAdaptiveTarget();
    testActualControllerOptInAndEligibility();
    testActualControllerRateBandsRestoreProtection();
    testFastControllerLayerRoundTrip(7);testFastControllerLayerRoundTrip(8);
    testFastControllerLayerRoundTrip(8,true);
    testFastControllerLayerRoundTrip(9);testFastControllerLayerRoundTrip(9,true);
    testFastControllerLayerRoundTrip(10);testFastControllerLayerRoundTrip(10,true);
    testFastControllerLayerRoundTrip(10,false,2);testFastControllerLayerRoundTrip(10,true,2);
    testOrdinaryVariationKeepsLayer();
    testRateBaselineStepIsNotClockDrift();
    testActualControllerInputMissReleaseFeedbackApi();
    testActualControllerRepeatedZeroTimestampDrops();
    testActualControllerPostInputErrorsCannotInflateBuffer();
    testActualControllerRequiresAnInputHitch();
#endif
    std::fprintf(stderr,"input percentile lifecycle integration tests: %s (%d failures)\n",failures?"FAIL":"PASS",failures);
    return failures?1:0;
}
