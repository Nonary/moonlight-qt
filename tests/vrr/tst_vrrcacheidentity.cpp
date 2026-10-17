#include "../../app/streaming/video/ffmpeg-renderers/pacer/vrrpacingworker.h"
#include "../../app/streaming/video/ffmpeg-renderers/pacer/vrr/profile.h"
#include <QCoreApplication>
#include <QTemporaryDir>
#include <QCryptographicHash>
#include <cassert>
#include <chrono>
#include <cstdio>
extern "C" uint64_t LiGetMicroseconds(void) {
    return uint64_t(std::chrono::duration_cast<std::chrono::microseconds>(
        std::chrono::steady_clock::now().time_since_epoch()).count());
}
static QString key(const VrrTimingParameters& p) {
    QString context="same-device-codec-settings";
    if(p.playoutInputClockRelativeHistory>=4)
      context+=QStringLiteral("|input-receive-drift-clock=%1").arg(p.playoutInputClockRelativeHistory);
    return QCryptographicHash::hash(context.toUtf8(),QCryptographicHash::Sha256).toHex();
}
int main(int argc,char**argv) {
    QCoreApplication app(argc,argv);
    qunsetenv("MOONLIGHT_VRR_EXPERIMENTAL_ADAPTIVE_BUFFER");
    qunsetenv("MOONLIGHT_VRR_INPUT_ACCOUNTING");
    qunsetenv("MOONLIGHT_VRR_BOUNDED_CATCHUP");
    VrrSessionConfig raw;raw.streamRateHz=116;raw.displayRefreshHz=120;raw.latencyMode=1;
    raw.timingOptions={1000,9950,120,500};
    auto before=vrrTimingParametersForSession(raw,false,false);
    auto effective=VrrPacingWorker::resolvedInputConfig(raw);
    auto after=vrrTimingParametersForSession(effective,false,false);
    assert(before.playoutInputClockRelativeHistory==0&&after.playoutInputClockRelativeHistory==4);
    assert(after.playoutInputPercentileTargetPerMillion==995000);
    assert(after.playoutInputPercentileWindowUs==120000000);
    assert(after.playoutInputPercentileToleranceUs==500);
    QTemporaryDir dir;assert(dir.isValid());const QString path=dir.path()+"/seed.json";
    assert(Vrr13::saveStartDelay(path,key(before),8378));
    const auto incorrectlyShared=Vrr13::loadStartDelay(path,key(before));
    const auto separated=Vrr13::loadStartDelay(path,key(after));
    assert(incorrectlyShared==8378&&separated==0);
    qputenv("MOONLIGHT_VRR_EXPERIMENTAL_ADAPTIVE_BUFFER","0");
    const auto legacy=vrrTimingParametersForSession(VrrPacingWorker::resolvedInputConfig(raw),false,false);
    assert(legacy.playoutInputClockRelativeHistory==0&&legacy.playoutInputPercentileEnabled==0);
    std::printf("raw_cache_clock=%llu live_clock=%llu old_namespace_seed=%llu corrected_namespace_seed=%llu; target=995000 window=120000000 tolerance=500; legacy diagnostic preserved\n",
     (unsigned long long)before.playoutInputClockRelativeHistory,(unsigned long long)after.playoutInputClockRelativeHistory,
     (unsigned long long)incorrectlyShared,(unsigned long long)separated);
}
