#include <QtTest>
#include <QTemporaryDir>
#include <QNetworkProxy>
#include "backend/nvhttp.h"
#include "backend/poweractions.h"
#include "tlsfixture.h"

// A power request entering ordinary stream launch is an immediate test failure.
extern "C" const char* LiGetLaunchUrlQueryParameters(void) { qFatal("Power request entered stream launch"); return ""; }

class PowerActionTest : public QObject {
    Q_OBJECT
    QTemporaryDir settings;
    TlsCredentials paired, wrong;
    QSslConfiguration originalConfig;
    const QByteArray token = QByteArray(64, 'a');
    QByteArray capabilities(QByteArray overrides = {}) const {
        return "<root status_code=\"200\"><PowerActionVersion>1</PowerActionVersion><SleepSupported>1</SleepSupported>"
            "<SleepAllowed>1</SleepAllowed><SleepAvailable>1</SleepAvailable><SleepActiveSessionPolicy>idle-only</SleepActiveSessionPolicy>"
            "<SleepToken>" + token + "</SleepToken>" + overrides + "</root>";
    }
    static QByteArray acceptance() { return "<root status_code=\"202\"><SleepAccepted>1</SleepAccepted></root>"; }
    NvHTTP* http(TlsServer& server, bool hasPair = true, QNetworkAccessManager* legacyNetwork = nullptr) {
        return new NvHTTP(NvAddress("127.0.0.1", 47989), server.serverPort(),
                          hasPair ? paired.certificate : QSslCertificate(), true, legacyNetwork);
    }
    void trust(QList<QSslCertificate> certificates) {
        auto config = originalConfig;
        config.setCaCertificates(certificates);
        QSslConfiguration::setDefaultConfiguration(config);
    }
private slots:
    void initTestCase() {
        QVERIFY(settings.isValid());
        QCoreApplication::setOrganizationName("MoonlightPowerActionTest");
        QCoreApplication::setApplicationName("Isolated");
        QSettings::setDefaultFormat(QSettings::IniFormat);
        QSettings::setPath(QSettings::IniFormat, QSettings::UserScope, settings.path());
        paired = TlsCredentials::generate(settings.path(), "paired");
        wrong = TlsCredentials::generate(settings.path(), "wrong");
        QVERIFY(!paired.certificate.isNull()); QVERIFY(!wrong.certificate.isNull());
        QVERIFY(paired.certificate != wrong.certificate);
        originalConfig = QSslConfiguration::defaultConfiguration();
    }
    void init() { trust({}); }
    void cleanup() { QSslConfiguration::setDefaultConfiguration(originalConfig); }
    void exactDedicatedRequestsAndAcceptance() {
        TlsServer server(paired); server.results = {{capabilities()}, {acceptance(), 202}};
        QScopedPointer<NvHTTP> client(http(server)); client->requestHostSleep();
        QCOMPARE(server.requests.size(), 2);
        const auto get = server.requests[0]; const auto post = server.requests[1];
        QCOMPARE(get.line, QByteArray("GET /power-actions/v1 HTTP/1.1"));
        QCOMPARE(post.line, "POST /power-actions/v1/sleep?token=" + token + " HTTP/1.1");
        QVERIFY(post.body.isEmpty()); QCOMPARE(server.posts(), 1);
    }
    void oldUnsupportedAndBusyHostsNeverPost() {
        for (const QByteArray xml : {QByteArray("<root status_code=\"200\"><hostname>Stock</hostname></root>"),
             capabilities().replace("<SleepSupported>1", "<SleepSupported>0"),
             capabilities().replace("<SleepAllowed>1", "<SleepAllowed>0"),
             capabilities().replace("<SleepAvailable>1", "<SleepAvailable>0"),
             capabilities().replace("idle-only", "terminate-all"),
             capabilities().replace(token, QByteArray(64, 'z')), capabilities("<SleepToken>duplicate</SleepToken>")}) {
            TlsServer server(paired); server.results = {{xml}};
            QScopedPointer<NvHTTP> client(http(server));
            QVERIFY_EXCEPTION_THROWN(client->requestHostSleep(), std::exception);
            QCOMPARE(server.requests.size(), 1); QCOMPARE(server.posts(), 0);
        }
    }
    void revokedAndRedirectNeverPost() {
        for (const int status : {403, 307}) {
            TlsServer server(paired); server.results = {{"refused", status}};
            QScopedPointer<NvHTTP> client(http(server));
            QVERIFY_EXCEPTION_THROWN(client->requestHostSleep(), std::exception);
            QCOMPARE(server.requests.size(), 1); QCOMPARE(server.posts(), 0);
        }
    }
    void lostAcceptanceNeverRetries() {
        TlsServer server(paired); server.results = {{capabilities()}, {{}, 0, true, true}};
        QScopedPointer<NvHTTP> client(http(server));
        QVERIFY_EXCEPTION_THROWN(client->requestHostSleep(), QtNetworkReplyException);
        QCOMPARE(server.requests.size(), 2);
        QTest::qWait(20); QCOMPARE(server.requests.size(), 2); QCOMPARE(server.posts(), 1);
    }
    void lostDiscoveryNeverRetriesOrPosts() {
        TlsServer server(paired); server.results = {{{}, 0, true, true}};
        QScopedPointer<NvHTTP> client(http(server));
        QVERIFY_EXCEPTION_THROWN(client->requestHostSleep(), QtNetworkReplyException);
        QCOMPARE(server.requests.size(), 1); QCOMPARE(server.posts(), 0);
        QTest::qWait(20); QCOMPARE(server.requests.size(), 1);
    }
    void redirectedAcceptanceNeverReplaysPost() {
        TlsServer server(paired); server.results = {{capabilities()}, {"redirect", 307}};
        QScopedPointer<NvHTTP> client(http(server));
        QVERIFY_EXCEPTION_THROWN(client->requestHostSleep(), std::exception);
        QCOMPARE(server.requests.size(), 2); QCOMPARE(server.posts(), 1);
        QTest::qWait(20); QCOMPARE(server.requests.size(), 2);
    }
    void malformedOrWrongAcceptanceRejected() {
        for (const QByteArray xml : {QByteArray("<root status_code=\"200\"><SleepAccepted>1</SleepAccepted></root>"),
             QByteArray("<root status_code=\"202\"><SleepAccepted>0</SleepAccepted></root>"),
             QByteArray("<root status_code=\"202\"><SleepAccepted>1</SleepAccepted>"), acceptance() + "<root/>"}) {
            TlsServer server(paired); server.results = {{capabilities()}, {xml, 202}};
            QScopedPointer<NvHTTP> client(http(server));
            QVERIFY_EXCEPTION_THROWN(client->requestHostSleep(), std::exception);
            QCOMPARE(server.requests.size(), 2);
        }
    }
    void unpairedNeverSends() {
        TlsServer server(paired); QScopedPointer<NvHTTP> client(http(server, false));
        QVERIFY_EXCEPTION_THROWN(client->requestHostSleep(), std::exception);
        QVERIFY(server.requests.isEmpty()); QCOMPARE(server.connections, 0);
    }
    void trustedWrongPeerNeverReceivesGetOrPost() {
        trust({wrong.certificate});
        TlsServer server(wrong); server.results = {{capabilities()}, {acceptance(), 202}};
        QScopedPointer<NvHTTP> client(http(server));
        QVERIFY_EXCEPTION_THROWN(client->requestHostSleep(), std::exception);
        QCOMPARE(server.connections, 1); QVERIFY(server.requests.isEmpty()); QCOMPARE(server.posts(), 0);
    }
    void untrustedWrongPeerNeverReceivesGetOrPost() {
        TlsServer server(wrong); server.results = {{capabilities()}, {acceptance(), 202}};
        QScopedPointer<NvHTTP> client(http(server));
        QVERIFY_EXCEPTION_THROWN(client->requestHostSleep(), std::exception);
        QCOMPARE(server.connections, 1); QVERIFY(server.requests.isEmpty()); QCOMPARE(server.posts(), 0);
    }
    void savedMatchingTrustedPeerAccepted() {
        trust({paired.certificate});
        TlsServer server(paired); server.results = {{capabilities()}, {acceptance(), 202}};
        QScopedPointer<NvHTTP> client(http(server)); client->requestHostSleep();
        QCOMPARE(server.requests.size(), 2); QCOMPARE(server.posts(), 1);
    }
    void keepAliveOfferStillUsesFreshPinnedConnectionForPost() {
        trust({paired.certificate});
        TlsServer server(paired); server.results = {{capabilities(), 200, false}, {acceptance(), 202, false}};
        QScopedPointer<NvHTTP> client(http(server)); client->requestHostSleep();
        QCOMPARE(server.requests.size(), 2); QCOMPARE(server.connections, 2);
        QVERIFY(server.requests[0].connection != server.requests[1].connection);
    }
    void newConnectionWithChangedTrustedCertificateNeverReceivesPost() {
        trust({paired.certificate, wrong.certificate});
        TlsServer server(paired); server.results = {{capabilities(), 200, true, false,
            [&] { server.credentials = wrong; }}, {acceptance(), 202}};
        QScopedPointer<NvHTTP> client(http(server));
        QVERIFY_EXCEPTION_THROWN(client->requestHostSleep(), std::exception);
        QCOMPARE(server.connections, 2); QCOMPARE(server.requests.size(), 1); QCOMPARE(server.posts(), 0);
    }
    void endpointAndCertificateChangesDoNotRetargetConfirmedTransaction() {
        trust({paired.certificate, wrong.certificate});
        TlsServer server(paired), other(wrong); QScopedPointer<NvHTTP> client(http(server));
        server.results = {{capabilities(), 200, true, false, [&] {
            client->setAddress(NvAddress("localhost", 47989)); client->setHttpsPort(other.serverPort());
            client->setServerCert(wrong.certificate);
        }}, {acceptance(), 202}};
        client->requestHostSleep();
        QCOMPARE(server.requests.size(), 2); QCOMPARE(server.posts(), 1);
        QCOMPARE(other.connections, 0); QVERIFY(other.requests.isEmpty());
    }
    void changedSavedPinCannotAuthorizeReplacementPeerMidTransaction() {
        trust({paired.certificate, wrong.certificate});
        TlsServer server(paired); QScopedPointer<NvHTTP> client(http(server));
        server.results = {{capabilities(), 200, true, false, [&] {
            client->setServerCert(wrong.certificate); server.credentials = wrong;
        }}, {acceptance(), 202}};
        QVERIFY_EXCEPTION_THROWN(client->requestHostSleep(), std::exception);
        QCOMPARE(server.connections, 2); QCOMPARE(server.requests.size(), 1); QCOMPARE(server.posts(), 0);
    }
    void reusedWrongTrustedLegacyConnectionCannotBeBorrowed() {
        trust({wrong.certificate});
        TlsServer server(wrong); server.results = {{"first", 200, false}, {"second", 200, false}};
        QNetworkAccessManager legacy; legacy.setProxy(QNetworkProxy::NoProxy);
        int encrypted = 0, sslErrors = 0;
        connect(&legacy, &QNetworkAccessManager::encrypted, [&] { ++encrypted; });
        connect(&legacy, &QNetworkAccessManager::sslErrors, [&] { ++sslErrors; });
        const QUrl url(QString("https://127.0.0.1:%1/warm").arg(server.serverPort()));
        for (const auto body : {QByteArray("first"), QByteArray("second")}) {
            QNetworkRequest request(url);
            request.setSslConfiguration(IdentityManager::get()->getSslConfig());
#if QT_VERSION >= QT_VERSION_CHECK(6, 0, 0)
            request.setAttribute(QNetworkRequest::Http2AllowedAttribute, false);
#endif
            auto* reply = legacy.get(request); QSignalSpy finished(reply, &QNetworkReply::finished);
            QVERIFY(finished.wait(2000)); QCOMPARE(reply->error(), QNetworkReply::NoError);
            QCOMPARE(reply->readAll(), body); delete reply;
        }
        QCOMPARE(server.connections, 1); QCOMPARE(encrypted, 1); QCOMPARE(sslErrors, 0);
        QScopedPointer<NvHTTP> client(http(server, true, &legacy));
        QVERIFY_EXCEPTION_THROWN(client->requestHostSleep(), std::exception);
        QCOMPARE(server.requests.size(), 2); QCOMPARE(server.posts(), 0); QCOMPARE(server.connections, 2);
    }
    void genericLegacyTrustedPeerBehaviorIsPreserved() {
        trust({wrong.certificate}); TlsServer server(wrong); server.results = {{"legacy"}};
        QScopedPointer<NvHTTP> client(http(server));
        QCOMPARE(client->openConnectionToString(client->m_BaseUrlHttps, "serverinfo", {}, 2000, NvHTTP::NVLL_NONE), QString("legacy"));
        QCOMPARE(server.requests.size(), 1); QVERIFY(server.requests[0].line.contains("uniqueid="));
    }
    void discoveryMetadataAndDowngrade() {
        QVERIFY(PowerActions::advertised(capabilities("<DisplayModes><Mode>nested</Mode></DisplayModes>")));
        QVERIFY(!PowerActions::advertised("<root status_code=\"200\"><hostname>Old</hostname></root>"));
        QVERIFY(!PowerActions::advertised(capabilities().replace("<PowerActionVersion>1", "<PowerActionVersion>2")));
        QVERIFY(!PowerActions::advertised(capabilities("<SleepAllowed>1</SleepAllowed>")));
        QVERIFY(!PowerActions::advertised(capabilities().chopped(7)));
    }
};
QTEST_GUILESS_MAIN(PowerActionTest)
#include "tst_poweractions.moc"
