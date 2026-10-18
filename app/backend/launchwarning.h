#pragma once

#include <QCoreApplication>
#include <QString>
#include <QXmlStreamReader>

namespace LaunchWarning {

enum class Code { None, VirtualDisplayDisabledByClient };

// This optional extension never changes launch success. Only recognized codes
// in a small, well-formed successful response can supply local overlay text.
inline Code parse(const QString& response)
{
    if (response.size() > 16384) return Code::None;
    QXmlStreamReader xml(response);
    bool root = false, success = false, started = false, warningSeen = false;
    Code result = Code::None;
    int depth = 0;
    while (!xml.atEnd()) {
        const auto token = xml.readNext();
        if (token == QXmlStreamReader::DTD) return Code::None;
        if (token == QXmlStreamReader::StartElement) {
            ++depth;
            if (depth == 1) {
                if (root || xml.name() != QStringLiteral("root") || !xml.namespaceUri().isEmpty()) return Code::None;
                root = true;
                success = xml.attributes().value(QStringLiteral("status_code")) == QStringLiteral("200");
            }
            else if (depth == 2 && xml.namespaceUri().isEmpty()) {
                const auto name = xml.name().toString();
                if (name == QStringLiteral("clientWarning")) {
                    if (warningSeen) return Code::None;
                    warningSeen = true;
                    if (xml.readElementText() == QStringLiteral("virtual_display_disabled_by_client"))
                        result = Code::VirtualDisplayDisabledByClient;
                    --depth;
                }
                else if (name == QStringLiteral("gamesession") || name == QStringLiteral("resume")) {
                    started = xml.readElementText() == QStringLiteral("1") || started;
                    --depth;
                }
            }
        }
        else if (token == QXmlStreamReader::EndElement) --depth;
    }
    return !xml.hasError() && root && success && started ? result : Code::None;
}

inline QString text(Code code)
{
    if (code != Code::VirtualDisplayDisabledByClient) return {};
    return QCoreApplication::translate("Session",
        "Your client disabled the virtual display requested by the host.\n"
        "Streaming continues with the existing display. To use a virtual display, enable it in your client.");
}

}
