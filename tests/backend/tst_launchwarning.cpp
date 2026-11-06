#include "backend/launchwarning.h"
#include <QCoreApplication>
#include <QFile>
#include <cassert>
#include <iostream>

int main(int argc, char** argv)
{
    QCoreApplication app(argc, argv);
    using LaunchWarning::Code;
    const QString code = QStringLiteral("<clientWarning>virtual_display_disabled_by_client</clientWarning>");
    const auto response = [&](QString body, QString status = QStringLiteral("200")) {
        return QStringLiteral("<root status_code=\"") + status + QStringLiteral("\">") + body + QStringLiteral("</root>");
    };
    int checks = 0;
    const auto check = [&](const QString& xml, Code expected) {
        assert(LaunchWarning::parse(xml).code == expected);
        ++checks;
    };
    const auto known = Code::VirtualDisplayDisabledByClient;
    check(response(QStringLiteral("<gamesession>1</gamesession>") + code), known);
    check(response(code + QStringLiteral("<resume>1</resume>")), known);
    check(response(QStringLiteral("<resume>1</resume><sessionUrl0>rtsp://host:48010</sessionUrl0>") + code), known);
    for (auto status : {QStringLiteral("400"), QStringLiteral("401"), QStringLiteral("403"), QStringLiteral("409"), QStringLiteral("503"), QStringLiteral("bad")})
        check(response(QStringLiteral("<resume>1</resume>") + code, status), Code::None);
    check(response(QStringLiteral("<resume>0</resume>") + code), Code::None);
    check(response(code), Code::None);
    check(response(QStringLiteral("<resume>1</resume>")), Code::None);
    check(response(QStringLiteral("<resume>1</resume><clientWarning>future_code</clientWarning>")), Code::None);
    check(response(QStringLiteral("<resume>1</resume><clientWarning>&lt;b&gt;host markup&lt;/b&gt;</clientWarning>")), Code::None);
    check(response(QStringLiteral("<resume>1</resume><other>") + code + QStringLiteral("</other>")), Code::None);
    check(response(QStringLiteral("<resume>1</resume>") + code + code), Code::None);
    check(response(QStringLiteral("<resume>1</resume><clientWarning>future_code</clientWarning>") + code), Code::None);
    check(response(QStringLiteral("<resume>1</resume><clientWarning><nested/>virtual_display_disabled_by_client</clientWarning>")), Code::None);
    check(response(QStringLiteral("<resume>1</resume>") + code).chopped(1), Code::None);
    check(response(QStringLiteral("<resume>1</resume>") + code) + QStringLiteral("<extra/>"), Code::None);
    check(QStringLiteral("<!DOCTYPE root [<!ENTITY code 'virtual_display_disabled_by_client'>]><root status_code=\"200\"><resume>1</resume><clientWarning>&code;</clientWarning></root>"), Code::None);
    check(response(QStringLiteral("<resume>1</resume>") + code + QString(16384, 'x')), Code::None);
    check(QStringLiteral("<root xmlns=\"other\" status_code=\"200\"><resume>1</resume>") + code + QStringLiteral("</root>"), Code::None);
    check(QStringLiteral(""), Code::None);
    // Unknown codes show the host's message, so new host warnings need no client
    // release. Recognized codes keep their local, translatable text.
    const auto warning = [&](const QString& xml) { ++checks; return LaunchWarning::parse(xml); };
    const QString hostText = QStringLiteral("The host could not create a virtual display.");
    const QString message = QStringLiteral("<clientWarningMessage>") + hostText + QStringLiteral("</clientWarningMessage>");
    const QString unknownCode = QStringLiteral("<clientWarning>virtual_display_unavailable</clientWarning>");
    auto w = warning(response(QStringLiteral("<gamesession>1</gamesession>") + code + message));
    assert(w.code == known && w.message == hostText && LaunchWarning::text(w) == LaunchWarning::text(known));
    w = warning(response(QStringLiteral("<resume>1</resume>") + unknownCode + message));
    assert(w.code == Code::None && LaunchWarning::text(w) == hostText);
    w = warning(response(QStringLiteral("<resume>1</resume>") + code));
    assert(w.message.isEmpty() && LaunchWarning::text(w) == LaunchWarning::text(known));
    assert(LaunchWarning::text(warning(response(QStringLiteral("<resume>1</resume>") + message))).isEmpty());
    assert(LaunchWarning::text(warning(response(QStringLiteral("<resume>1</resume>") + code + message, QStringLiteral("503")))).isEmpty());
    assert(LaunchWarning::text(warning(response(QStringLiteral("<resume>0</resume>") + code + message))).isEmpty());
    assert(LaunchWarning::text(warning(response(QStringLiteral("<resume>1</resume>") + code + message + message))).isEmpty());
    assert(LaunchWarning::text(warning(response(QStringLiteral("<resume>1</resume>") + code +
                                                QStringLiteral("<clientWarningMessage><b>x</b></clientWarningMessage>")))).isEmpty());
    w = warning(response(QStringLiteral("<resume>1</resume>") + code +
                         QStringLiteral("<clientWarningMessage>  Line one\n\tLine two\u202E  </clientWarningMessage>")));
    assert(w.message == QStringLiteral("Line one\n Line two"));
    w = warning(response(QStringLiteral("<resume>1</resume>") + code + QStringLiteral("<clientWarningMessage>") +
                         QString(LaunchWarning::MaxMessageBytes + 1, QLatin1Char('x')) + QStringLiteral("</clientWarningMessage>")));
    assert(w.message.isEmpty() && LaunchWarning::text(w) == LaunchWarning::text(known));
    // The cap counts UTF-8 bytes: 200 two-byte characters fit, 300 do not.
    const auto sized = [&](int count) {
        return warning(response(QStringLiteral("<resume>1</resume>") + unknownCode + QStringLiteral("<clientWarningMessage>") +
                                QString(count, QChar(0x00E9)) + QStringLiteral("</clientWarningMessage>"))).message;
    };
    assert(sized(200).size() == 200 && sized(300).isEmpty());
    const auto cleaned = [&](const QString& body) {
        return warning(response(QStringLiteral("<resume>1</resume>") + unknownCode + QStringLiteral("<clientWarningMessage>") +
                                body + QStringLiteral("</clientWarningMessage>"))).message;
    };
    assert(cleaned(QStringLiteral("a\u2028b\u2029c")) == QStringLiteral("a\nb\nc"));
    assert(cleaned(QStringLiteral("1\n2\n3\n4\n5")) == QStringLiteral("1\n2\n3\n4 5"));
    assert(cleaned(QStringLiteral("x\u200Dy\u200Cz")) == QStringLiteral("x\u200Dy\u200Cz"));
    assert(cleaned(QStringLiteral("tag\U000E0001ged")) == QStringLiteral("tagged"));
    w = warning(response(QStringLiteral("<resume>1</resume>") + code +
                         QStringLiteral("<clientWarningMessage>&lt;b&gt;plain&lt;/b&gt;</clientWarningMessage>")));
    assert(w.message == QStringLiteral("<b>plain</b>")); // Drawn literally: the overlay renders plain text.
    assert(LaunchWarning::text(Code::None).isEmpty());
    assert(!LaunchWarning::text(known).isEmpty());
    assert(!LaunchWarning::text(known).contains(QStringLiteral("<")));
    // Parse each response afresh: a successful warning must not leak through a
    // later ordinary or rejected response, even when the caller reuses storage.
    check(response(QStringLiteral("<resume>1</resume>")), Code::None);
    if (argc == 2) {
        QFile hostResponse(QString::fromLocal8Bit(argv[1]));
        assert(hostResponse.open(QIODevice::ReadOnly));
        check(QString::fromUtf8(hostResponse.readAll()), known);
    }
    std::cout << checks << " launch/resume response cases passed\n";
}
