#pragma once

#include <QCoreApplication>
#include <QString>
#include <QXmlStreamReader>

namespace LaunchWarning {

enum class Code { None, VirtualDisplayDisabledByClient };

// Longer host text is treated as missing, so a host cannot flood the overlay.
// Measured in UTF-8 bytes: the overlay draws from a fixed 1024-byte buffer.
constexpr int MaxMessageBytes = 512;
constexpr int MaxMessageLineBreaks = 3;

struct Warning {
    Code code = Code::None;
    QString message;
};

// Host text is shown as plain text. Drop control and format characters, which
// could reorder or hide text, but keep the joiners that emoji and many scripts
// need. Limit line breaks so a message cannot make an oversized overlay.
inline QString sanitizeMessage(const QString& raw)
{
    QString text;
    text.reserve(raw.size());
    int lineBreaks = 0;
    for (const char32_t codePoint : raw.toUcs4()) {
        const bool lineBreak = codePoint == U'\n' || codePoint == 0x2028 || codePoint == 0x2029;
        if (lineBreak && ++lineBreaks <= MaxMessageLineBreaks) {
            text.append(QLatin1Char('\n'));
            continue;
        }
        if (lineBreak || codePoint == U'\r' || codePoint == U'\t') {
            text.append(QLatin1Char(' '));
            continue;
        }
        const auto category = QChar::category(codePoint);
        const bool joiner = codePoint == 0x200C || codePoint == 0x200D;
        if (category == QChar::Other_Control || (category == QChar::Other_Format && !joiner)) continue;
        text.append(QString::fromUcs4(&codePoint, 1));
    }
    text = text.trimmed();
    return text.toUtf8().size() > MaxMessageBytes ? QString {} : text;
}

// This optional extension never changes launch success. A warning is read only
// from a small, well-formed successful response that started a stream. The
// host's message is optional; recognized codes fall back to local text.
inline Warning parse(const QString& response)
{
    if (response.size() > 16384) return {};
    QXmlStreamReader xml(response);
    bool root = false, success = false, started = false, warningSeen = false, messageSeen = false;
    Warning result;
    int depth = 0;
    while (!xml.atEnd()) {
        const auto token = xml.readNext();
        if (token == QXmlStreamReader::DTD) return {};
        if (token == QXmlStreamReader::StartElement) {
            ++depth;
            if (depth == 1) {
                if (root || xml.name() != QStringLiteral("root") || !xml.namespaceUri().isEmpty()) return {};
                root = true;
                success = xml.attributes().value(QStringLiteral("status_code")) == QStringLiteral("200");
            }
            else if (depth == 2 && xml.namespaceUri().isEmpty()) {
                const auto name = xml.name().toString();
                if (name == QStringLiteral("clientWarning")) {
                    if (warningSeen) return {};
                    warningSeen = true;
                    if (xml.readElementText() == QStringLiteral("virtual_display_disabled_by_client"))
                        result.code = Code::VirtualDisplayDisabledByClient;
                    --depth;
                }
                else if (name == QStringLiteral("clientWarningMessage")) {
                    if (messageSeen) return {};
                    messageSeen = true;
                    result.message = sanitizeMessage(xml.readElementText());
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
    // A message travels with a warning code; on its own it is not a warning.
    return !xml.hasError() && root && success && started && warningSeen ? result : Warning {};
}

inline QString text(Code code)
{
    if (code != Code::VirtualDisplayDisabledByClient) return {};
    return QCoreApplication::translate("Session",
        "Your client disabled the virtual display requested by the host.\n"
        "Streaming continues with the existing display. To use a virtual display, enable it in your client.");
}

// Recognized codes keep their local, translatable text. Any other warning shows
// the host's own explanation, so new host warnings need no client release.
inline QString text(const Warning& warning)
{
    const QString local = text(warning.code);
    return local.isEmpty() ? warning.message : local;
}

}
