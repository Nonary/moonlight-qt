#include <QtTest>
#include <QTemporaryDir>
#include <QUrlQuery>
#include <QPointer>
#include <cstring>
#include <functional>

#include "backend/profilemanager.h"
#include "backend/identitymanager.h"
#include "backend/nvhttp.h"
#include "settings/streamingpreferences.h"
#include "path.h"

// Profile storage, migration, lookup and preference loading are production code.
// Credential generation and platform probes are outside these regressions.
static int identityResetCount = 0;
static QString clientUid;
static QSslConfiguration clientSslConfiguration;
IdentityManager::IdentityManager() {}
IdentityManager* IdentityManager::get() { static IdentityManager identity; return &identity; }
void IdentityManager::reset() { ++identityResetCount; }
QString IdentityManager::getUniqueId() { return clientUid; }
QSslConfiguration IdentityManager::getSslConfig() { return clientSslConfiguration; }
extern "C" const char* LiGetLaunchUrlQueryParameters() { return ""; }
static QString boxArtDirectory;
QString Path::getBoxArtCacheDir() { return boxArtDirectory; }
namespace WMUtils {
bool isRunningWayland() { return false; }
bool isGpuSlow() { return false; }
}

class FixtureReply : public QNetworkReply
{
    QByteArray m_Body = "fixture response";
    qint64 m_Position = 0;
    int& m_Aborts;

    qint64 readData(char* data, qint64 length) override
    {
        const qint64 count = qMin(length, m_Body.size() - m_Position);
        if (count == 0) return -1;
        std::memcpy(data, m_Body.constData() + m_Position, static_cast<size_t>(count));
        m_Position += count;
        return count;
    }

public:
    FixtureReply(const QNetworkRequest& request, QObject* parent, int& aborts)
        : QNetworkReply(parent), m_Aborts(aborts)
    {
        setRequest(request);
        setUrl(request.url());
        setOperation(QNetworkAccessManager::GetOperation);
        open(QIODevice::ReadOnly);
    }

    qint64 bytesAvailable() const override { return m_Body.size() - m_Position + QNetworkReply::bytesAvailable(); }
    void complete() { setFinished(true); emit readyRead(); emit finished(); }
    void abort() override
    {
        ++m_Aborts;
        setError(QNetworkReply::OperationCanceledError, "fixture canceled");
        setFinished(true);
        emit finished();
    }
};

class FixtureNetworkManager : public QNetworkAccessManager
{
    QNetworkReply* createRequest(Operation, const QNetworkRequest& request, QIODevice*) override
    {
        requests.append(request);
        auto* reply = new FixtureReply(request, this, aborts);
        pending = reply;
        if (onRequestCreated) onRequestCreated();
        if (autoComplete) QTimer::singleShot(0, reply, [reply] { reply->complete(); });
        return reply;
    }

public:
    QList<QNetworkRequest> requests;
    QPointer<FixtureReply> pending;
    int aborts = 0;
    bool autoComplete = true;
    std::function<void()> onRequestCreated;
};

class ProfilesTest : public QObject
{
    Q_OBJECT
    QTemporaryDir m_SettingsDirectory;
    ProfileManager* m_Manager = nullptr;
    const QString m_Guid = "030000005e0400008e02000000000000";
    const QString m_Mapping = "Test Controller,a:b0,b:b1,platform:Windows,";

    static QString request(NvHTTP& http)
    {
        return http.openConnectionToString(http.m_BaseUrlHttps, "fixture", QString(), 0, NvHTTP::NVLL_NONE);
    }

    static bool canceled(NvHTTP& http)
    {
        try { request(http); }
        catch (const QtNetworkReplyException& error) { return error.getError() == QNetworkReply::OperationCanceledError; }
        return false;
    }

private slots:
    void initTestCase()
    {
        QVERIFY(m_SettingsDirectory.isValid());
        QCoreApplication::setOrganizationName("MoonlightTests");
        QCoreApplication::setApplicationName("Profiles");
        QSettings::setDefaultFormat(QSettings::IniFormat);
        QSettings::setPath(QSettings::IniFormat, QSettings::UserScope, m_SettingsDirectory.path());
        QSettings::setPath(QSettings::IniFormat, QSettings::SystemScope, m_SettingsDirectory.path());
        boxArtDirectory = m_SettingsDirectory.filePath("boxart");

        QSettings legacy;
        legacy.setValue("renderer", StreamingPreferences::RS_VULKAN);
        legacy.setValue("enablevrr", true);
        legacy.setValue("vrrlatencyfix", false);
        legacy.setValue("vrrlatencymode", StreamingPreferences::VLM_LOW_LATENCY);
        legacy.setValue("smoothvrrframetiming", false);
        legacy.setValue("tracevrrframes", true);
        legacy.beginWriteArray("gcmapping", 1);
        legacy.setArrayIndex(0);
        legacy.setValue("guid", m_Guid);
        legacy.setValue("mapping", m_Mapping);
        legacy.endArray();
        legacy.sync();
        QCOMPARE(legacy.status(), QSettings::NoError);

        m_Manager = ProfileManager::get();
    }

    void inactivePreferencesUseDefaultsWithoutChangingStoredSettings()
    {
        QVERIFY(!ProfileManager::hasActiveProfile());
        const auto storedValues = [] {
            QSettings settings;
            QVariantMap values;
            for (const auto& key : settings.allKeys()) values.insert(key, settings.value(key));
            return values;
        };
        const QVariantMap before = storedValues();
        auto* preferences = StreamingPreferences::get();
        QCOMPARE(preferences->width, 1280);
        QCOMPARE(preferences->height, 720);
        QCOMPARE(preferences->fps, 60);
        QCOMPARE(preferences->rendererSelection, StreamingPreferences::RS_AUTO);
        QVERIFY(!preferences->enableVrr);
        QCOMPARE(preferences->vrrLatencyMode, StreamingPreferences::VLM_BALANCED_TARGET);

        preferences->width = 2048;
        preferences->rendererSelection = StreamingPreferences::RS_METAL;
        preferences->enableVrr = true;
        preferences->save();
        preferences->reload();
        QCOMPARE(preferences->width, 1280);
        QCOMPARE(preferences->rendererSelection, StreamingPreferences::RS_AUTO);
        QVERIFY(!preferences->enableVrr);
        QCOMPARE(storedValues(), before);
    }

    void migratesLegacyRendererControllerAndVrrSettings()
    {
        QVERIFY(m_Manager->activateDefaultProfile());
        QCOMPARE(StreamingPreferences::get()->rendererSelection, StreamingPreferences::RS_VULKAN);
        QVERIFY(StreamingPreferences::get()->enableVrr);
        QCOMPARE(StreamingPreferences::get()->vrrLatencyMode, StreamingPreferences::VLM_LOW_LATENCY);
        QVERIFY(!StreamingPreferences::get()->smoothVrrFrameTiming);
        QVERIFY(StreamingPreferences::get()->traceVrrFrames);

        QSettings profile;
        ProfileManager::beginProfileSettings(profile, m_Manager->defaultProfileId());
        // The retired checkbox remains a migration input for installations
        // without vrrlatencymode. Keep it even when the newer key takes precedence.
        QVERIFY(profile.contains("vrrlatencyfix"));
        QVERIFY(!profile.value("vrrlatencyfix").toBool());
        QCOMPARE(profile.beginReadArray("gcmapping"), 1);
        profile.setArrayIndex(0);
        QCOMPARE(profile.value("guid").toString(), m_Guid);
        QCOMPARE(profile.value("mapping").toString(), m_Mapping);
        profile.endArray();
    }

    void profileIdsTakePrecedenceOverDisplayNames()
    {
        const QString originalId = m_Manager->defaultProfileId();
        QVERIFY(m_Manager->renameProfile(originalId, "Primary"));
        const QString secondId = m_Manager->createProfile(originalId);
        QVERIFY(!secondId.isEmpty());

        QVERIFY(m_Manager->activateProfile(secondId));
        QString error;
        QVERIFY2(m_Manager->activateProfileByNameOrId(originalId, &error), qPrintable(error));
        QCOMPARE(ProfileManager::activeProfileId(), originalId);
        QVERIFY2(m_Manager->activateProfileByNameOrId("  " + originalId.toUpper() + "  ", &error), qPrintable(error));
        QCOMPARE(ProfileManager::activeProfileId(), originalId);

        QVERIFY2(m_Manager->activateProfileByNameOrId(secondId.toUpper(), &error), qPrintable(error));
        QCOMPARE(ProfileManager::activeProfileId(), secondId);
        QVERIFY2(m_Manager->activateProfileByNameOrId("pRiMaRy", &error), qPrintable(error));
        QCOMPARE(ProfileManager::activeProfileId(), originalId);
        QVERIFY(!m_Manager->activateProfileByNameOrId("unknown profile", &error));
        QVERIFY(!error.isEmpty());
        QCOMPARE(ProfileManager::activeProfileId(), originalId);
    }

    void profileChangeSignalsBracketStateReplacementAndRemoval()
    {
        QVERIFY(m_Manager->activateDefaultProfile());
        const QString previousId = ProfileManager::activeProfileId();
        const QString nextId = m_Manager->createProfile("Lifecycle");
        QVERIFY(!nextId.isEmpty());
        const QString markerKey = "profiles/" + nextId + "/lifecycle-marker";
        QSettings settings;
        settings.setValue("profiles/" + nextId + "/renderer", StreamingPreferences::RS_AUTO);
        settings.setValue(markerKey, "present");

        QObject observer;
        QStringList phases;
        QString beforeId, afterId;
        StreamingPreferences::RendererSelection beforeRenderer, afterRenderer;
        int beforeResets = -1, afterResets = -1;
        bool markerBefore = false;
        bool requestsBefore = true, requestsAfter = false;
        connect(m_Manager, &ProfileManager::activeProfileAboutToChange, &observer, [&] {
            phases.append("before");
            beforeId = ProfileManager::activeProfileId();
            beforeRenderer = StreamingPreferences::get()->rendererSelection;
            beforeResets = identityResetCount;
            markerBefore = QSettings().contains(markerKey);
            requestsBefore = m_Manager->requestsAllowed();
        });
        connect(m_Manager, &ProfileManager::activeProfileChanged, &observer, [&] {
            phases.append("after");
            afterId = ProfileManager::activeProfileId();
            afterRenderer = StreamingPreferences::get()->rendererSelection;
            afterResets = identityResetCount;
            requestsAfter = m_Manager->requestsAllowed();
        });

        const int initialResets = identityResetCount;
        QVERIFY(m_Manager->activateProfile(nextId));
        QCOMPARE(phases, (QStringList{"before", "after"}));
        QCOMPARE(beforeId, previousId);
        QCOMPARE(beforeRenderer, StreamingPreferences::RS_VULKAN);
        QCOMPARE(beforeResets, initialResets);
        QVERIFY(!requestsBefore);
        QCOMPARE(afterId, nextId);
        QCOMPARE(afterRenderer, StreamingPreferences::RS_AUTO);
        QCOMPARE(afterResets, initialResets + 1);
        QVERIFY(requestsAfter);

        phases.clear();
        QVERIFY(m_Manager->activateProfile(nextId));
        QVERIFY(phases.isEmpty());
        QCOMPARE(identityResetCount, initialResets + 1);

        QVERIFY(m_Manager->removeProfile(nextId));
        QCOMPARE(phases, (QStringList{"before", "after"}));
        QCOMPARE(beforeId, nextId);
        QCOMPARE(beforeRenderer, StreamingPreferences::RS_AUTO);
        QCOMPARE(beforeResets, initialResets + 1);
        QVERIFY(markerBefore);
        QVERIFY(afterId.isEmpty());
        QCOMPARE(afterResets, initialResets + 2);
        QVERIFY(!QSettings().contains(markerKey));
        QVERIFY(!requestsAfter);

        QVERIFY(m_Manager->activateDefaultProfile());
        phases.clear();
        const int resetsBeforeShutdown = identityResetCount;
        m_Manager->deactivateProfile();
        QCOMPARE(phases, (QStringList{"before", "after"}));
        QCOMPARE(beforeId, previousId);
        QCOMPARE(beforeRenderer, StreamingPreferences::RS_VULKAN);
        QCOMPARE(beforeResets, resetsBeforeShutdown);
        QVERIFY(!requestsBefore);
        QVERIFY(afterId.isEmpty());
        QCOMPARE(afterResets, resetsBeforeShutdown + 1);
        QVERIFY(!requestsAfter);
        QVERIFY(!m_Manager->requestsAllowed());
        phases.clear();
        m_Manager->deactivateProfile();
        QVERIFY(phases.isEmpty());
        QCOMPARE(identityResetCount, resetsBeforeShutdown + 1);
    }

    void httpKeepsOriginUidAndSslConfiguration()
    {
        QVERIFY(m_Manager->activateDefaultProfile());
        clientUid = "origin-profile";
        clientSslConfiguration.setProtocol(QSsl::TlsV1_2);
        FixtureNetworkManager network;
        NvHTTP origin(NvAddress("fixture.invalid", DEFAULT_HTTP_PORT), DEFAULT_HTTPS_PORT, QSslCertificate(), true, &network);

        clientUid = "new-profile";
        clientSslConfiguration.setProtocol(QSsl::TlsV1_3);
        QCOMPARE(request(origin), QString("fixture response"));
        QCOMPARE(network.requests.size(), 1);
        QCOMPARE(QUrlQuery(network.requests.last().url()).queryItemValue("uniqueid"), QString("origin-profile"));
        QCOMPARE(network.requests.last().sslConfiguration().protocol(), QSsl::TlsV1_2);

        NvHTTP fresh(NvAddress("fixture.invalid", DEFAULT_HTTP_PORT), DEFAULT_HTTPS_PORT, QSslCertificate(), true, &network);
        QCOMPARE(request(fresh), QString("fixture response"));
        QCOMPARE(QUrlQuery(network.requests.last().url()).queryItemValue("uniqueid"), QString("new-profile"));
        QCOMPARE(network.requests.last().sslConfiguration().protocol(), QSsl::TlsV1_3);
    }

    void httpCancellationBlocksPendingAndStaleRequestsUntilFreshClient()
    {
        QVERIFY(m_Manager->activateDefaultProfile());
        m_Manager->resumeRequests();
        FixtureNetworkManager network;
        network.autoComplete = false;
        NvHTTP pending(NvAddress("fixture.invalid", DEFAULT_HTTP_PORT), DEFAULT_HTTPS_PORT, QSslCertificate(), true, &network);
        bool watchdogFired = false;
        QTimer watchdog;
        watchdog.setSingleShot(true);
        connect(&watchdog, &QTimer::timeout, &network, [&] {
            watchdogFired = true;
            if (network.pending) network.pending->abort();
        });
        watchdog.start(1000);
        QTimer::singleShot(0, m_Manager, [this] { m_Manager->suspendRequests(); });
        QVERIFY(canceled(pending));
        watchdog.stop();
        QVERIFY(!watchdogFired);
        QCOMPARE(network.aborts, 1);

        m_Manager->resumeRequests();
        network.autoComplete = true;
        NvHTTP stale(NvAddress("fixture.invalid", DEFAULT_HTTP_PORT), DEFAULT_HTTPS_PORT, QSslCertificate(), true, &network);
        m_Manager->suspendRequests();
        NvHTTP duringSuspension(NvAddress("fixture.invalid", DEFAULT_HTTP_PORT), DEFAULT_HTTPS_PORT, QSslCertificate(), true, &network);
        QVERIFY(canceled(stale));
        QVERIFY(canceled(duringSuspension));
        QCOMPARE(network.requests.size(), 1);
        m_Manager->resumeRequests();
        QVERIFY(canceled(stale));
        QVERIFY(canceled(duringSuspension));
        QCOMPARE(network.requests.size(), 1);

        // Suspend synchronously while get() creates the reply, before NvHTTP
        // connects its event loop. A missed signal must not leave that loop running.
        NvHTTP interrupted(NvAddress("fixture.invalid", DEFAULT_HTTP_PORT), DEFAULT_HTTPS_PORT, QSslCertificate(), true, &network);
        network.autoComplete = false;
        network.onRequestCreated = [this] { m_Manager->suspendRequests(); };
        watchdog.start(1000);
        QVERIFY(canceled(interrupted));
        watchdog.stop();
        QVERIFY(!watchdogFired);
        QCOMPARE(network.aborts, 2);

        m_Manager->resumeRequests();
        NvHTTP completed(NvAddress("fixture.invalid", DEFAULT_HTTP_PORT), DEFAULT_HTTPS_PORT, QSslCertificate(), true, &network);
        network.onRequestCreated = [this, &network] {
            m_Manager->suspendRequests();
            network.pending->complete();
        };
        QVERIFY(canceled(completed));

        m_Manager->resumeRequests();
        network.onRequestCreated = {};
        network.autoComplete = true;
        NvHTTP fresh(NvAddress("fixture.invalid", DEFAULT_HTTP_PORT), DEFAULT_HTTPS_PORT, QSslCertificate(), true, &network);
        QCOMPARE(request(fresh), QString("fixture response"));
        QCOMPARE(network.requests.size(), 4);
    }
};

QTEST_GUILESS_MAIN(ProfilesTest)
#include "tst_profiles.moc"
