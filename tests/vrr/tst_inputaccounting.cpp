#include "../../app/streaming/video/ffmpeg-renderers/pacer/vrr/vrrtimingcontroller.h"
#include "../../app/streaming/video/timinggraph.h"

#include <algorithm>
#include <cstdio>
#include <random>
#include <utility>

namespace {
int failures = 0;
void check(bool condition, const char* message)
{
    if (!condition) { std::fprintf(stderr, "FAIL: %s\n", message); ++failures; }
}

struct DriftResult { double meanBufferUs; uint64_t missed, eligible, maximumUs; };

DriftResult runDrift(double ppm, bool sparseEarly)
{
    VrrSessionConfig config;
    config.streamRateHz = 116; config.displayRefreshHz = 120;
    config.latencyMode = 1; config.smoothFrameTiming = false;
    config.timingOptions = {1000, 9950, 60, 500};
    config = vrrProductionSessionConfig(config);
    auto policy = vrrTimingParametersForSession(config);
    policy.playoutDelayStartSeedUs = 1000;
    VrrTimingController controller(config, true, policy);
    double sourceUs = 1000000, sum = 0;
    uint64_t submitted = 0, counted = 0, maximum = 0;
    uint32_t rtp = 0;
    std::mt19937_64 rng(42);
    std::gamma_distribution<double> jitter(2.0, 400.0);
    std::uniform_real_distribution<double> sample(0, 1);
    for (uint64_t frame = 0; frame < 9000; ++frame) {
        sourceUs += 8600.0 * (1 + ppm * 1e-6);
        rtp += 774;
        const bool early = sparseEarly && sample(rng) < 0.01;
        const uint64_t ready = early ? uint64_t(sourceUs) + 300 :
            uint64_t(sourceUs) + (sparseEarly ? 1800 : 800) + uint64_t(jitter(rng)) +
            (sample(rng) < 0.003 ? 4000 : 0);
        PacedFrame input(nullptr, int(frame), rtp, true, ready);
        input.setDeliveryTimeline(ready - 800, ready - 600, ready - 500);
        const auto decision = controller.schedule(input, std::max(ready, submitted));
        const uint64_t prepared = std::max(ready, decision.renderStartUs) + 500;
        submitted = std::max(prepared, decision.targetUs);
        controller.notePreparationDuration(500, 0, prepared);
        controller.noteSchedulerDelays(0, 0, true);
        controller.noteSubmission(true, false, submitted, submitted);
        if (frame >= 3500) {
            const auto buffer = controller.intervalStats().inputPercentile.currentUs;
            sum += buffer; maximum = std::max(maximum, buffer); ++counted;
        }
    }
    const auto stats = controller.intervalStats().inputPercentile;
    return {sum / counted, stats.missed, stats.eligible, maximum};
}

void testClockDrift()
{
    for (bool sparse : {false, true}) {
        const auto steady = runDrift(0, sparse);
        for (double ppm : {-50.0, 50.0}) {
            const auto drift = runDrift(ppm, sparse);
            std::printf("clock drift %+.0f ppm sparse=%d: mean %.0f vs %.0f us, misses %llu/%llu\n",
                        ppm, sparse, drift.meanBufferUs, steady.meanBufferUs,
                        static_cast<unsigned long long>(drift.missed),
                        static_cast<unsigned long long>(drift.eligible));
            check(drift.eligible > 5000 && drift.missed <= drift.eligible,
                  "drift keeps real input observations and valid coverage accounting");
            check(drift.meanBufferUs <= steady.meanBufferUs * 1.5 + 2500,
                  "clock drift does not ratchet standing input protection");
            check(drift.maximumUs <= 10000,
                  "clock drift remains inside the configured source-relative buffer capacity");
        }
    }
}

void testRecoveryCredit()
{
    VrrSessionConfig config;
    config.streamRateHz = 76; config.displayRefreshHz = 120;
    config.timingOptions = {4000, 9995, 300, 500};
    config = vrrProductionSessionConfig(config);
    for (const auto& [target, share] : {std::pair<int, uint64_t>{9900, 1000},
                                         {9950, 500}, {9995, 50}, {9000, 1000}}) {
        auto selected = config;
        selected.timingOptions.targetHundredths = target;
        const auto policy = vrrTimingParametersForSession(selected);
        VrrTimingController controller(selected, true, policy);
        const auto physical = controller.recoveryHeadroomUs();
        const auto expected = physical * share / 1000;
        const auto actual = controller.inputRecoveryCreditUs();
        check(policy.playoutInputRecoveryCreditRoomPerMille == share &&
              actual <= physical && actual + 1 >= expected && actual <= expected + 1,
              "input recovery credit follows the selected goal and never exceeds physical room");
    }
    VrrSessionConfig nearCeiling = config;
    nearCeiling.streamRateHz = 116;
    VrrTimingController controller(nearCeiling, true, vrrTimingParametersForSession(nearCeiling));
    check(controller.inputRecoveryCreditUs() <= controller.recoveryHeadroomUs() &&
          controller.inputRecoveryCreditUs() < 500,
          "near the display ceiling recovery credit remains physically bounded");
}

void testGraphAccounting()
{
    Overlay::TimingGraphPoint point;
    point.sourceTimingValid = true; point.sourceTimeUs = 100000;
    point.bufferUs = 1000; point.inputPercentileEnabled = true;
    point.producerEligible = true; point.inputToleranceUs = 500;
    point.inputVrrHeadroomUs = 500; point.producerRequiredUs = -1000;
    const auto sample = Overlay::bufferGraphSample(point);
    check(sample.producerUs == -1500 && !sample.producerMissed() && sample.bufferUs == 1000,
          "signed readiness remains separate from applied input buffering");
}
struct SpanRun{uint64_t peakBufferUs,missed;double tailLatencyUs;};
SpanRun sendSpanRun(uint64_t normalization,bool bursts,uint64_t latencyDropUs=0,bool sustainedSmall=false) {
 VrrSessionConfig cfg; cfg.latencyMode=1; cfg.streamRateHz=120; cfg.displayRefreshHz=120; cfg=vrrProductionSessionConfig(cfg);
 auto policy=vrrTimingParametersForSession(cfg);policy.playoutDelayStartSeedUs=1000;
 policy.playoutOffsetSendSpanNormalization=normalization;
 VrrTimingController c(cfg,true,policy);uint64_t slot=1000000,submitted=0,peak=0;uint32_t pts=0;
 std::mt19937_64 rng(11);std::uniform_int_distribution<uint64_t> jitter(0,500);
 const uint64_t period=9091,frames=110*240;double tail=0;uint64_t tailCount=0,missedBefore=0;
 for(uint64_t frame=0;frame<frames;++frame) {
  slot+=period;pts+=818;const uint64_t second=frame/110;
  const bool late=second>=120;
  const bool small=(bursts&&late&&second%30<2&&frame%2==0)||(sustainedSmall&&late&&second<150);
  const uint64_t receive=slot+2000-(small?500:0)+jitter(rng)-(late?latencyDropUs:0);
  const uint64_t reassembled=receive+(small?300:3350),submit=reassembled+100,ready=submit+600;
  PacedFrame f(nullptr,int(frame),pts,true,ready);f.setDeliveryTimeline(receive,reassembled,submit);
  const auto d=c.schedule(f,std::max(ready,submitted));
  const uint64_t prepared=std::max(ready,d.renderStartUs)+500;
  submitted=std::max(prepared,d.targetUs);c.notePreparationDuration(500,0,prepared);
  c.noteSchedulerDelays(0,0,true);c.noteSubmission(true,false,submitted,submitted);
  if(frame+1==120*110) missedBefore=c.intervalStats().inputPercentile.missed;
  if(late) peak=std::max(peak,d.playoutDelayUs);
  if(second>=225&&!small){tail+=double(submitted-ready);++tailCount;}
 }
 return {peak,c.intervalStats().inputPercentile.missed-missedBefore,tail/tailCount};
}
void smallFramesDoNotMoveOffset() {
 const auto quiet=sendSpanRun(1,false),raw=sendSpanRun(0,true),fixed=sendSpanRun(1,true);
 std::printf("small-frame bursts: buffer peak quiet %llu, raw %llu (%llu misses), normalized %llu (%llu misses)\n",
  (unsigned long long)quiet.peakBufferUs,(unsigned long long)raw.peakBufferUs,(unsigned long long)raw.missed,
  (unsigned long long)fixed.peakBufferUs,(unsigned long long)fixed.missed);
 check(raw.peakBufferUs>=quiet.peakBufferUs+1500,"fixture reproduces the small-frame buffer rise without normalization");
 check(fixed.peakBufferUs<=quiet.peakBufferUs+300&&fixed.missed<=quiet.missed+5,"small early frames do not pull the offset ahead of full frames");
 const auto dropRaw=sendSpanRun(0,false,3000),dropFixed=sendSpanRun(1,false,3000);
 std::printf("lasting 3 ms latency drop: tail ready-to-present raw %.0f us, normalized %.0f us\n",dropRaw.tailLatencyUs,dropFixed.tailLatencyUs);
 check(dropFixed.tailLatencyUs<=dropRaw.tailLatencyUs+200,"a real latency drop of full frames is still adopted");
 const auto allSmallRaw=sendSpanRun(0,false,0,true),allSmallFixed=sendSpanRun(1,false,0,true);
 check(allSmallFixed.peakBufferUs<=allSmallRaw.peakBufferUs+300,"a sustained small-frame scene becomes the typical span");
}

}

int main()
{
    smallFramesDoNotMoveOffset();
    testRecoveryCredit();
    testGraphAccounting();
    testClockDrift();
    std::printf("current input accounting: %d failures\n", failures);
    return failures ? 1 : 0;
}
