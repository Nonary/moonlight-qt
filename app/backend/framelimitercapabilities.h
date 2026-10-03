#pragma once

#include <QString>
#include <QXmlStreamReader>
#include <cstdint>

// Optional /serverinfo extension. Configuration is not proof of a successful
// limiter application to a particular game. Zero FPS means follow stream FPS.
struct FrameLimiterCapabilities
{
    bool supported = false;
    bool enabled = false;
    bool virtualDisplayEnabled = false;
    uint32_t fpsLimitMilliHz = 0;

    static FrameLimiterCapabilities fromServerInfo(const QString& serverInfo)
    {
        FrameLimiterCapabilities result;
        QXmlStreamReader xml(serverInfo);
        while (!xml.atEnd()) {
            if (xml.readNext() != QXmlStreamReader::StartElement) continue;
            const auto name = xml.name().toString();
            if (name != "FrameLimiterSupported" && name != "FrameLimiterEnabled" &&
                name != "VirtualDisplayFrameLimiterEnabled" &&
                name != "FrameLimiterFpsLimitMilliHz") continue;
            const auto value = xml.readElementText().trimmed();
            const bool flag = value == "1" || value.compare("true", Qt::CaseInsensitive) == 0;
            if (name == "FrameLimiterSupported") result.supported = flag;
            else if (name == "FrameLimiterEnabled") result.enabled = flag;
            else if (name == "VirtualDisplayFrameLimiterEnabled") result.virtualDisplayEnabled = flag;
            else result.fpsLimitMilliHz = value.toUInt();
        }
        return xml.hasError() ? FrameLimiterCapabilities{} : result;
    }
};
