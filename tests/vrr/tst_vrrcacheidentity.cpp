#include "assertions.h"
#include "../../app/streaming/video/ffmpeg-renderers/pacer/vrrpacingworker.h"
#include "../../app/streaming/video/ffmpeg-renderers/pacer/vrr/profile.h"
#include <QCoreApplication>
#include <QCryptographicHash>
#include <QTemporaryDir>
#include <cassert>
#include <chrono>
#include <cstdio>

extern "C" uint64_t LiGetMicroseconds(void) {
    return uint64_t(std::chrono::duration_cast<std::chrono::microseconds>(
        std::chrono::steady_clock::now().time_since_epoch()).count());
}

int main(int argc, char** argv) {
    QCoreApplication app(argc, argv);
    VrrSessionConfig raw;
    raw.streamRateHz = 116;
    raw.displayRefreshHz = 120;
    raw.latencyMode = 1;
    raw.timingOptions = {1000, 9950, 120, 500};
    const auto resolved = vrrProductionSessionConfig(raw);
    const auto policy = vrrTimingParametersForSession(resolved, false, false);
    assert(policy.playoutInputPercentileTargetPerMillion == 995000);
    assert(policy.playoutInputPercentileWindowUs == 120000000);
    const auto identity = [](const VrrTimingParameters& p, bool canLatch) {
        return Vrr13::calibrationKey("same-device-codec-settings",
            QByteArray::fromStdString(vrrCalibrationPolicy(p)), canLatch, true);
    };
    const auto key = identity(policy, false);
    QCryptographicHash previous(QCryptographicHash::Sha256);
    previous.addData(QByteArrayLiteral("vrr-calibration-v3\n"));
    previous.addData(QByteArrayLiteral("same-device-codec-settings"));
    previous.addData(QByteArrayLiteral("\ncan-latch=0\nallow-tearing=1"));
    previous.addData(QByteArray::fromStdString(vrrCalibrationPolicy(policy)));
    const auto previousKey = QString::fromLatin1(previous.result().toHex());
    assert(key != previousKey);
    QTemporaryDir dir;
    assert(dir.isValid());
    const auto path = dir.filePath("seed.json");
    assert(Vrr13::saveStartDelay(path, previousKey, 14492));
    assert(Vrr13::loadStartDelay(path, key) == 0);
    assert(Vrr13::saveStartDelay(path, key, 8378));
    assert(Vrr13::loadStartDelay(path, key) == 8378);
    assert(Vrr13::loadStartDelay(path, identity(policy, true)) == 0);
    auto changed = policy;
    changed.playoutInputPercentileTargetPerMillion = 990000;
    assert(Vrr13::loadStartDelay(path, identity(changed, false)) == 0);
    const char* switches[] = {"MOONLIGHT_VRR_EXPERIMENTAL_ADAPTIVE_BUFFER",
                              "MOONLIGHT_VRR_INPUT_ACCOUNTING", "MOONLIGHT_VRR_BOUNDED_CATCHUP"};
    for (unsigned mask = 0; mask < 8; ++mask) {
        for (unsigned i = 0; i < 3; ++i) {
            if (mask & (1U << i)) qputenv(switches[i], "0");
            else qunsetenv(switches[i]);
        }
        const auto candidate = vrrTimingParametersForSession(vrrProductionSessionConfig(raw), false, false);
#define VRR_ASSERT_SAME_PARAMETER(type, jsonName, memberName, defaultValue) assert(candidate.memberName == policy.memberName);
        VRR_TIMING_PARAMETER_FIELDS(VRR_ASSERT_SAME_PARAMETER)
#undef VRR_ASSERT_SAME_PARAMETER
        assert(identity(candidate, false) == key);
    }
    std::puts("Effective policy and presenter capabilities isolate calibration; retired switches ignored");
}
