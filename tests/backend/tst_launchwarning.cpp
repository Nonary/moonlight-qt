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
        assert(LaunchWarning::parse(xml) == expected);
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
