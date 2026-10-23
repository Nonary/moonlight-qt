#pragma once

#include <QHash>
#include <QRegularExpression>
#include <QString>
#include <QXmlStreamReader>
#include <stdexcept>

// Optional paired HTTPS extension. No app ID or streaming state is involved.
namespace PowerActions {
struct Response {
    int status = 0;
    QHash<QString, QString> fields;
};

inline Response parse(const QString& xml)
{
    QXmlStreamReader reader(xml);
    Response response;
    if (!reader.readNextStartElement() || reader.name() != QLatin1String("root"))
        throw std::runtime_error("Malformed power response");
    bool validStatus = false;
    response.status = reader.attributes().value("status_code").toInt(&validStatus);
    if (!validStatus) throw std::runtime_error("Missing power response status");
    while (reader.readNextStartElement()) {
        const QString name = reader.name().toString();
        if (!name.startsWith("Sleep") && name != "PowerActionVersion") {
            reader.skipCurrentElement();
            continue;
        }
        if (response.fields.contains(name)) throw std::runtime_error("Duplicate power response field");
        response.fields.insert(name, reader.readElementText(QXmlStreamReader::ErrorOnUnexpectedElement));
    }
    // Consume the complete document; truncated or trailing XML is not acceptance.
    while (!reader.atEnd()) reader.readNext();
    if (reader.hasError()) throw std::runtime_error("Malformed power response");
    return response;
}

inline bool advertised(const QString& xml)
{
    try {
        const auto response = parse(xml);
        return response.status == 200 &&
            response.fields.value("PowerActionVersion") == "1" &&
            response.fields.value("SleepSupported") == "1" &&
            response.fields.value("SleepAllowed") == "1";
    } catch (const std::exception&) { return false; }
}

inline QString confirmedToken(const QString& xml)
{
    const auto response = parse(xml);
    const auto token = response.fields.value("SleepToken");
    static const QRegularExpression tokenFormat(QStringLiteral("^[0-9a-f]{64}$"));
    if (response.status != 200 || !advertised(xml) ||
        response.fields.value("SleepAvailable") != "1" ||
        response.fields.value("SleepActiveSessionPolicy") != "idle-only" ||
        !tokenFormat.match(token).hasMatch())
        throw std::runtime_error("The host cannot sleep now. Check sleep permission and end all host sessions, then confirm again.");
    return token;
}

inline bool accepted(const QString& xml)
{
    const auto response = parse(xml);
    return response.status == 202 && response.fields.value("SleepAccepted") == "1";
}
}
