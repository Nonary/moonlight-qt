#include "../../app/backend/autoupdatechecker.h"

#include <QNetworkReply>
#include <QtTest>
#include <cstring>
#include <QSettings>
#include <QTemporaryDir>

class ReleaseReply : public QNetworkReply
{
public:
    ReleaseReply(const QByteArray& data, NetworkError error = NoError) : m_Data(data)
    {
        open(QIODevice::ReadOnly);
        setFinished(true);
        if (error != NoError) {
            setError(error, QStringLiteral("Update request failed"));
        }
    }

    void abort() override {}
    qint64 bytesAvailable() const override { return m_Data.size() - m_Offset + QNetworkReply::bytesAvailable(); }

protected:
    qint64 readData(char* data, qint64 maximum) override
    {
        const qint64 length = qMin(maximum, qint64(m_Data.size()) - m_Offset);
        if (length == 0) return -1;
        std::memcpy(data, m_Data.constData() + m_Offset, size_t(length));
        m_Offset += length;
        return length;
    }

private:
    QByteArray m_Data;
    qint64 m_Offset = 0;
};

class RecordingManager : public QNetworkAccessManager
{
public:
    QList<QUrl> requests;
protected:
    QNetworkReply* createRequest(Operation, const QNetworkRequest& request, QIODevice*) override
    {
        requests.append(request.url());
        auto* reply = new ReleaseReply("[]");
        reply->setParent(this);
        return reply;
    }
};

class AutoUpdateCheckerTest : public QObject
{
    Q_OBJECT

    static QJsonObject release(const QString& tag, const QString& asset, bool prerelease = false)
    {
        QString digest;
        if (tag == "v6.1.0-vrr17.1") digest = "sha256:7cf6a4700676c3ee6597a519016374e6176fb8936de1c591eddca9dcd71c53c7";
        if (tag == "v6.1.0-vrr18") digest = "sha256:900bf4e71bd8e4ff0fc7a01cd7ec08f073adc5da03a28d5968356b5dc72c795b";
        return {{"tag_name", tag}, {"draft", false}, {"prerelease", prerelease},
                {"assets", QJsonArray{QJsonObject{{"name", asset}, {"state", "uploaded"}, {"size", 100}, {"digest",digest}}}}};
    }

    static QVector<int> version(const QString& string, bool windowsProduct = true)
    {
        QVector<int> result;
        AutoUpdateChecker::parseVersion(string, result, windowsProduct);
        return result;
    }

    QTemporaryDir settingsDirectory;
private slots:
    void initTestCase() {
        QCoreApplication::setOrganizationName("MoonlightUpdateTests");
        QCoreApplication::setApplicationName("IsolatedSettings");
        QSettings::setDefaultFormat(QSettings::IniFormat);
        QSettings::setPath(QSettings::IniFormat, QSettings::UserScope, settingsDirectory.path());
    }
    void init() { QSettings().clear(); }
    void defaultsMigrationAndPersistence() {
        { AutoUpdateChecker checker; QVERIFY(checker.notifyPrereleases()); checker.setNotifyPrereleases(false); }
        { AutoUpdateChecker checker; QVERIFY(!checker.notifyPrereleases()); checker.setNotifyPrereleases(true); }
        AutoUpdateChecker checker; QVERIFY(checker.notifyPrereleases());
        QSettings settings; settings.setValue("notifyPrereleases", false); settings.sync();
        AutoUpdateChecker migrated; QVERIFY(!migrated.notifyPrereleases());
    }
    void stableOnlyAndDismissal() {
        const QJsonArray releases{release("v6.1.0-vrr18", "MoonlightPortable-x64-6.1.0-vrr18.zip", true),
            release("v6.3.0", "MoonlightPortable-x64-6.3.0.zip")};
        QCOMPARE(AutoUpdateChecker::findUpdate(releases, version("6.1.0-vrr17.1"), "windows", "x86_64", false).value("tag_name").toString(), QString("v6.3.0"));
        auto update = release("v6.1.0-vrr18", "MoonlightPortable-x64-6.1.0-vrr18.zip", true);
        { AutoUpdateChecker checker; checker.dismissUpdate("6.1.0-vrr18"); }
        AutoUpdateChecker checker; QSignalSpy notifications(&checker, &AutoUpdateChecker::onUpdateAvailable);
        checker.announce(update); checker.announce(update);
        QCOMPARE(notifications.size(), 0);
        checker.setNotifyPrereleases(false);
        checker.announce(release("v6.1.0-vrr19", "MoonlightPortable-x64-6.1.0-vrr19.zip", true));
        QCOMPARE(notifications.size(), 0);
        checker.announce(release("v6.3.0", "MoonlightPortable-x64-6.3.0.zip"));
        QCOMPARE(notifications.size(), 1);
    }
    void unverifiedManualWindowsPackageStaysQuiet() {
        auto replaced = release("v6.1.0-vrr18", "MoonlightPortable-x64-6.1.0-vrr18.zip", true);
        auto assets = replaced["assets"].toArray();
        auto package = assets[0].toObject(); package["digest"] = "sha256:unknown";
        assets[0] = package; replaced["assets"] = assets;
        QVERIFY(AutoUpdateChecker::findUpdate({replaced}, version("6.1.0-vrr17.1"), "windows", "x86_64").isEmpty());
        auto unknown = release("v6.1.0-vrr99", "MoonlightPortable-x64-6.1.0-vrr99.zip", true);
        QVERIFY(AutoUpdateChecker::findUpdate({unknown}, version("6.1.0-daily.181"), "windows", "x86_64").isEmpty());
        QVERIFY(AutoUpdateChecker::compareVersion(version("6.1.0-daily.181", false), version("6.2.0", false)) < 0);
    }
    void actualInstalledWindowsVersion() {
#ifdef Q_OS_WIN
        QCOMPARE(AutoUpdateChecker::installedWindowsVersion(), QVector<int>({6, 2, 171, 0, 0}));
#endif
    }
    static void embedIdentity(QJsonObject& release) {
        const auto tag = release.value("tag_name").toString();
        const auto v = tag.mid(1);
        const int sequence = v.section('.', -1).toInt();
        const QJsonObject metadata{{"schema_version", 1}, {"channel", "nightly"}, {"tag", tag},
            {"version", v}, {"nightly_sequence", sequence}, {"windows_product_version", QString("6.2.%1").arg(sequence)},
            {"source_commit", QString(40, 'a')}};
        release["body"] = "<!-- nightly-identity: " + QString::fromUtf8(QJsonDocument(metadata).toJson(QJsonDocument::Compact)) + " -->\n";
    }
    void nightlyIdentityAndCompletion() {
        auto nightly = release("v6.1.0-daily.181", "MoonlightPortable-x64-6.1.0-daily.181.zip", true);
        auto assets=nightly["assets"].toArray();
        for (const QString name : {"MoonlightSetup-6.1.0-daily.181.exe", "nightly-metadata.json", "SHA256SUMS"})
            assets.append(QJsonObject{{"name",name},{"state","uploaded"},{"size",100}});
        nightly["assets"]=assets;
        embedIdentity(nightly);
        QVERIFY(!AutoUpdateChecker::findUpdate({nightly}, version("6.1.0-vrr18"), "windows", "arm64").isEmpty());
        QVERIFY(AutoUpdateChecker::findUpdate({nightly}, version("6.1.0-vrr18"), "windows", "i386").isEmpty());
        QVERIFY(!AutoUpdateChecker::findUpdate({nightly}, version("6.1.0-vrr18"), "windows", "x86_64").isEmpty());
        QVERIFY(AutoUpdateChecker::findUpdate({nightly}, version("6.1.0-daily.182"), "windows", "x86_64").isEmpty());
        QVERIFY(AutoUpdateChecker::findUpdate({nightly}, version("6.1.0-vrr18"), "windows", "x86_64", false).isEmpty());
        QJsonObject metadata{{"schema_version",1},{"channel","nightly"},{"tag","v6.1.0-daily.181"},{"version","6.1.0-daily.181"},{"nightly_sequence",181},{"windows_product_version","6.2.181"},{"source_commit",QString(40,'a')}};
        QVERIFY(AutoUpdateChecker::validNightlyMetadata(nightly,metadata));
        metadata["source_commit"]="bad"; QVERIFY(!AutoUpdateChecker::validNightlyMetadata(nightly,metadata));
        nightly["assets"]=QJsonArray{}; QVERIFY(AutoUpdateChecker::findUpdate({nightly}, version("6.1.0-vrr18"), "windows", "x86_64").isEmpty());
        QCOMPARE(AutoUpdateChecker::compareVersion(version("6.1.0-vrr17.2"),version("6.1.0-daily.172")),0);
        QVERIFY(AutoUpdateChecker::compareVersion(version("6.1.0-daily.181"),version("6.2.0"))>0);
    }
    void nightlyAppImageCompletion()
    {
        auto nightly = release("v6.1.0-daily.182", "Moonlight-6.1.0-daily.182-x86_64.AppImage", true);
        const auto installed = version("6.1.0-daily.181", false);
        QVERIFY(AutoUpdateChecker::findUpdate({nightly}, installed, "appimage", "x86_64").isEmpty());
        auto assets = nightly["assets"].toArray();
        for (const QString name : {"nightly-metadata.json", "SHA256SUMS"})
            assets.append(QJsonObject{{"name", name}, {"state", "uploaded"}, {"size", 100}});
        nightly["assets"] = assets;
        embedIdentity(nightly);
        QCOMPARE(AutoUpdateChecker::findUpdate({nightly}, installed, "appimage", "x86_64")
                     .value("tag_name").toString(), QString("v6.1.0-daily.182"));
        QVERIFY(AutoUpdateChecker::findUpdate({nightly}, installed, "appimage", "arm64").isEmpty());
        QVERIFY(AutoUpdateChecker::findUpdate({nightly}, installed, "appimage", "x86_64", false).isEmpty());
        auto aarch64 = assets;
        aarch64.append(QJsonObject{{"name", "Moonlight-6.1.0-daily.182-aarch64.AppImage"}, {"state", "uploaded"}, {"size", 100}});
        auto multiarch = nightly;
        multiarch["assets"] = aarch64;
        embedIdentity(multiarch);
        QCOMPARE(AutoUpdateChecker::findUpdate({multiarch}, installed, "appimage", "arm64")
                     .value("tag_name").toString(), QString("v6.1.0-daily.182"));
        QCOMPARE(AutoUpdateChecker::findUpdate({multiarch}, installed, "appimage", "x86_64")
                     .value("tag_name").toString(), QString("v6.1.0-daily.182"));
        QVERIFY(AutoUpdateChecker::findUpdate({nightly}, version("6.1.0-daily.182", false), "appimage", "x86_64").isEmpty());
        auto incomplete = assets[0].toObject();
        incomplete["state"] = "new";
        assets[0] = incomplete;
        nightly["assets"] = assets;
        embedIdentity(nightly);
        QVERIFY(AutoUpdateChecker::findUpdate({nightly}, installed, "appimage", "x86_64").isEmpty());
    }
    void nightlyAppFamilyMigration()
    {
        QCOMPARE(version("6.2.0-daily.183"), QVector<int>({6, 2, 183, 0, 0}));
        QCOMPARE(version("6.2.0-daily.183", false), QVector<int>({6, 2, 0, 183, 0}));
        QCOMPARE(version("6.1.0-daily.183"), version("6.2.0-daily.183"));
        QVERIFY(AutoUpdateChecker::compareVersion(version("6.2.0-daily.183"), version("6.1.0-daily.182")) > 0);
        QVERIFY(AutoUpdateChecker::compareVersion(version("6.2.0-daily.183", false), version("6.1.0-daily.182", false)) > 0);
        QVERIFY(AutoUpdateChecker::compareVersion(version("6.2.0"), version("6.1.0-daily.182")) < 0);

        auto nightly = release("v6.2.0-daily.183", "MoonlightPortable-x64-6.2.0-daily.183.zip", true);
        auto assets = nightly["assets"].toArray();
        for (const QString name : {"MoonlightSetup-6.2.0-daily.183.exe", "Moonlight-6.2.0-daily.183.dmg",
                                   "Moonlight-6.2.0-daily.183-x86_64.AppImage", "nightly-metadata.json", "SHA256SUMS"})
            assets.append(QJsonObject{{"name", name}, {"state", "uploaded"}, {"size", 100}});
        nightly["assets"] = assets;
        embedIdentity(nightly);
        QCOMPARE(AutoUpdateChecker::findUpdate({nightly}, version("6.1.0-daily.182"), "windows", "x86_64")
                     .value("tag_name").toString(), QString("v6.2.0-daily.183"));
        QVERIFY(!AutoUpdateChecker::findUpdate({nightly}, version("6.1.0-daily.182"), "windows", "arm64").isEmpty());
        for (const QString platform : {"osx", "appimage"}) {
            QVERIFY(!AutoUpdateChecker::findUpdate({nightly}, version("6.1.0-daily.182", false), platform, "x86_64").isEmpty());
            QVERIFY(AutoUpdateChecker::findUpdate({nightly}, version("6.2.0-daily.183", false), platform, "x86_64").isEmpty());
            QVERIFY(AutoUpdateChecker::findUpdate({nightly}, version("6.2.0-daily.184", false), platform, "x86_64").isEmpty());
        }
        QJsonObject metadata{{"schema_version", 1}, {"channel", "nightly"}, {"tag", "v6.2.0-daily.183"},
                             {"version", "6.2.0-daily.183"}, {"nightly_sequence", 183},
                             {"windows_product_version", "6.2.183"}, {"source_commit", QString(40, 'a')}};
        QVERIFY(AutoUpdateChecker::validNightlyMetadata(nightly, metadata));
        metadata["windows_product_version"] = "6.2.182";
        QVERIFY(!AutoUpdateChecker::validNightlyMetadata(nightly, metadata));
        for (const QString invalid : {"6.2.0-daily.0", "6.2.0-daily.65536", "6.2.0-daily.183.1",
                                      "6.2.1-daily.183", "6.3.0-daily.183", "6.2.0-vrr19"})
            QVERIFY(version(invalid).isEmpty());
    }
    void apiIdentityFailsClosedWithoutAssetRequests() {
        auto nightly = release("v6.2.0-daily.183", "MoonlightPortable-x64-6.2.0-daily.183.zip", true);
        auto assets = nightly["assets"].toArray();
        for (const QString name : {"MoonlightSetup-6.2.0-daily.183.exe", "SHA256SUMS"})
            assets.append(QJsonObject{{"name", name}, {"state", "uploaded"}, {"size", 100}});
        nightly["assets"] = assets;
        embedIdentity(nightly);
        QVERIFY(!AutoUpdateChecker::findUpdate({nightly}, version("6.1.0-daily.182"), "windows", "x86_64").isEmpty());
        auto duplicate = nightly;
        duplicate["body"] = nightly["body"].toString() + nightly["body"].toString();
        QVERIFY(AutoUpdateChecker::findUpdate({duplicate}, version("6.1.0-daily.182"), "windows", "x86_64").isEmpty());
        auto* manager = new RecordingManager;
        AutoUpdateChecker checker;
        delete checker.m_Nam;
        checker.m_Nam = manager;
        checker.m_Releases = {nightly};
        checker.m_CurrentVersionQuad = version("6.1.0-daily.182");
        checker.finishSelection();
        QCOMPARE(manager->requests.size(), 0);
        QVERIFY(!checker.m_Checking);
        nightly.remove("body");
        QVERIFY(AutoUpdateChecker::findUpdate({nightly}, version("6.1.0-daily.182"), "windows", "x86_64").isEmpty());
    }
    void versionOrdering()
    {
        const QStringList ordered{"6.1.0", "v6.1.0-vrr9", "6.1.0-vrr10", "6.1.0-vrr17",
                                  "6.1.0-vrr17.1", "6.1.0-vrr17.2", "6.1.0-vrr18", "6.1.0-daily.181", "6.3.0"};
        for (int i = 1; i < ordered.size(); ++i) {
            QCOMPARE(AutoUpdateChecker::compareVersion(version(ordered[i - 1]), version(ordered[i])), -1);
            QCOMPARE(AutoUpdateChecker::compareVersion(version(ordered[i]), version(ordered[i - 1])), 1);
        }
        QCOMPARE(AutoUpdateChecker::compareVersion(version("v6.1.0-vrr18"), version("6.1.0-vrr18.0")), 0);
        for (const QString& invalid : {"6.1.0-vrr-lite", "abcdef", "6.1.bad", "6.1.0-vrr18junk",
                                       "6.1.0-vrr99999999999999999999", "6.1.0\n", ""}) {
            QVERIFY2(version(invalid).isEmpty(), qPrintable(invalid));
        }
    }

    void publishedPrereleasesAndNumericSelection()
    {
        auto draft = release("v6.1.0-vrr20", "MoonlightPortable-x64-6.1.0-vrr20.zip");
        draft["draft"] = true;
        const QJsonArray releases{
            release("v6.1.0-vrr17.1", "MoonlightPortable-x64-6.1.0-vrr17.1.zip"),
            draft,
            release("v6.1.0-vrr18", "MoonlightPortable-x64-6.1.0-vrr18.zip", true),
            release("v6.1.0-vrr19", "Moonlight-6.1.0-vrr19-x86_64.AppImage"),
            release("v6.1.0-vrr9", "MoonlightPortable-x64-6.1.0-vrr9.zip")};
        QCOMPARE(AutoUpdateChecker::findUpdate(releases, version("6.1.0-vrr17"), "windows", "x86_64")
                 .value("tag_name").toString(), QString("v6.1.0-vrr18"));
        QVERIFY(AutoUpdateChecker::findUpdate(releases, version("6.1.0-vrr18"), "windows", "x86_64").isEmpty());
        QVERIFY(AutoUpdateChecker::findUpdate(releases, version("6.1.0-vrr21"), "windows", "x86_64").isEmpty());
        QVERIFY(AutoUpdateChecker::findUpdate(releases, version("6.1.0-vrr-lite"), "windows", "x86_64").isEmpty());
    }

    void platformAndArchitecture()
    {
        const QJsonArray releases{
            release("v6.1.0-vrr18", "MoonlightPortable-x64-6.1.0-vrr18.zip"),
            release("v6.1.0-vrr19", "MoonlightPortable-arm64-6.1.0-vrr19.zip"),
            release("v6.1.0-vrr20", "Moonlight-6.1.0-vrr20-x86_64.AppImage"),
            release("v6.1.0-vrr21", "Moonlight-6.1.0-vrr21.dmg"),
            release("v6.1.0-vrr22", "Moonlight-SteamLink-6.1.0-vrr22.zip"),
            release("v6.1.0-vrr23", "MoonlightPortable-x86-6.1.0-vrr23.zip")};
        const auto current = version("6.1.0-vrr17.1");
        auto selectedTag = [&](const QString& platform, const QString& arch) {
            return AutoUpdateChecker::findUpdate(releases, platform == "windows" ? current : version("6.1.0-vrr17.1", false), platform, arch).value("tag_name").toString();
        };
        QCOMPARE(selectedTag("windows", "x86_64"), QString("v6.1.0-vrr18"));
        QVERIFY(selectedTag("windows", "arm64").isEmpty());
        QVERIFY(selectedTag("windows", "i386").isEmpty());
        QCOMPARE(selectedTag("appimage", "x86_64"), QString("v6.1.0-vrr20"));
        QVERIFY(selectedTag("appimage", "arm64").isEmpty());
        QCOMPARE(selectedTag("osx", "arm64"), QString("v6.1.0-vrr21"));
        QCOMPARE(selectedTag("steamlink", "arm"), QString("v6.1.0-vrr22"));
        QVERIFY(selectedTag("linux", "x86_64").isEmpty());
    }

    void malformedAndIncompleteReleases()
    {
        auto missingDraft = release("v6.1.0-vrr18", "MoonlightPortable-x64-6.1.0-vrr18.zip");
        missingDraft.remove("draft");
        auto incomplete = release("v6.1.0-vrr19", "MoonlightPortable-x64-6.1.0-vrr19.zip");
        incomplete["assets"] = QJsonArray{QJsonObject{{"name", "MoonlightPortable-x64-6.1.0-vrr19.zip"},
                                                    {"state", "new"}, {"size", 0}}};
        const QJsonArray releases{QJsonValue("invalid"), QJsonObject{}, missingDraft, incomplete,
            release("v6.1.0-vrr20oops", "MoonlightPortable-x64-6.1.0-vrr20oops.zip"),
            // The upstream manifest format must never produce an update.
            QJsonObject{{"platform", "windows"}, {"arch", "x86_64"}, {"version", "99.0.0"},
                        {"browser_url", "https://github.com/moonlight-stream/moonlight-qt/releases"}}};
        QVERIFY(AutoUpdateChecker::findUpdate(releases, version("6.1.0-vrr17.1"), "windows", "x86_64").isEmpty());
    }

    void notificationUsesOwnRepository()
    {
        AutoUpdateChecker checker;
        QSignalSpy notifications(&checker, &AutoUpdateChecker::onUpdateAvailable);
        auto update = release("v6.1.0-vrr18", "MoonlightPortable-x64-6.1.0-vrr18.zip");
        update["html_url"] = "https://github.com/moonlight-stream/moonlight-qt/releases";
        auto* reply = new ReleaseReply(QJsonDocument(QJsonArray{update}).toJson());
        checker.handleUpdateCheckRequestFinished(reply);
#if defined(Q_OS_WIN)
        QCOMPARE(notifications.size(), 1);
        QCOMPARE(notifications[0][0].toString(), QString("6.1.0-vrr18"));
        QCOMPARE(notifications[0][1].toString(), QString("https://github.com/Nonary/moonlight-qt/releases/tag/v6.1.0-vrr18"));
#else
        QCOMPARE(notifications.size(), 0);
#endif
    }

    void paginationWaitsForAllPages() {
        AutoUpdateChecker checker;
        delete checker.m_Nam;
        auto* manager = new RecordingManager;
        manager->setParent(&checker);
        checker.m_Nam = manager;
        QSignalSpy notifications(&checker, &AutoUpdateChecker::onUpdateAvailable);
        QJsonArray page;
        for (int i = 0; i < 100; ++i) page.append(release("v6.3.0", "MoonlightPortable-x64-6.3.0.zip"));
        checker.handleUpdateCheckRequestFinished(new ReleaseReply(QJsonDocument(page).toJson()));
        QCOMPARE(notifications.size(), 0);
        QCOMPARE(manager->requests.size(), 1);
        QVERIFY(manager->requests[0].query().contains("page=2"));
        checker.handleUpdateCheckRequestFinished(new ReleaseReply("{}", QNetworkReply::ContentAccessDenied));
        QCOMPARE(notifications.size(), 0);

        AutoUpdateChecker complete;
        delete complete.m_Nam;
        complete.m_Nam = new RecordingManager;
        complete.m_Nam->setParent(&complete);
        QSignalSpy completed(&complete, &AutoUpdateChecker::onUpdateAvailable);
        complete.handleUpdateCheckRequestFinished(new ReleaseReply(QJsonDocument(page).toJson()));
        complete.handleUpdateCheckRequestFinished(new ReleaseReply("[]"));
#if defined(Q_OS_WIN)
        QCOMPARE(completed.size(), 1);
#endif
    }
    void failedResponsesStayQuiet()
    {
        for (const QByteArray& data : {QByteArray("bad json"), QByteArray("{\"message\":\"API rate limit exceeded\"}"), QByteArray("[]")}) {
            AutoUpdateChecker checker;
            QSignalSpy notifications(&checker, &AutoUpdateChecker::onUpdateAvailable);
            checker.handleUpdateCheckRequestFinished(new ReleaseReply(data));
            QCOMPARE(notifications.size(), 0);
        }
        AutoUpdateChecker checker;
        QSignalSpy notifications(&checker, &AutoUpdateChecker::onUpdateAvailable);
        checker.handleUpdateCheckRequestFinished(new ReleaseReply("[]", QNetworkReply::ContentAccessDenied));
        QCOMPARE(notifications.size(), 0);
    }
};

QTEST_GUILESS_MAIN(AutoUpdateCheckerTest)
#include "tst_autoupdatechecker.moc"
