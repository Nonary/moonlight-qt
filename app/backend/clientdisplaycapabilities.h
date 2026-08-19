#pragma once

#include <QString>

#include <optional>

struct ClientDisplayCapabilitySource
{
    QString source;
    double peakLuminanceNits = 0.0;
};

struct ClientDisplayCapabilities
{
    std::optional<ClientDisplayCapabilitySource> calibrated;
    std::optional<ClientDisplayCapabilitySource> edid;

    static std::optional<int> normalizePeakLuminance(double nits);
};
