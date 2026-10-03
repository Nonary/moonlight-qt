#include "clientdisplaycapabilities.h"

#include <cmath>

namespace
{
constexpr double kMinPeakLuminanceNits = 1.0;
constexpr double kMaxPeakLuminanceNits = 100000.0;
}

std::optional<int> ClientDisplayCapabilities::normalizePeakLuminance(const double nits)
{
    if (!std::isfinite(nits) || nits < kMinPeakLuminanceNits ||
            nits > kMaxPeakLuminanceNits) {
        return std::nullopt;
    }

    return static_cast<int>(std::round(nits));
}

QString ClientDisplayCapabilities::hdrPeakQueryArguments(
    const QString& verb, int hostVersion, bool hdrEnabled,
    int calibratedNits, int edidNits)
{
    if (hostVersion != 1 || !hdrEnabled ||
        (verb != QStringLiteral("launch") && verb != QStringLiteral("resume"))) {
        return {};
    }

    QString arguments;
    if (normalizePeakLuminance(calibratedNits)) {
        arguments += QStringLiteral("&clientHdrPeakCalibrated=") + QString::number(calibratedNits);
    }
    if (normalizePeakLuminance(edidNits)) {
        arguments += QStringLiteral("&clientHdrPeakEdid=") + QString::number(edidNits);
    }
    return arguments;
}
