#include "autoupdatechecker.h"

#include <QNetworkReply>
#include <QJsonDocument>
#include <QRegularExpression>
#include <QSettings>
#include <limits>
#ifdef Q_OS_WIN
#include <qt_windows.h>
#include <QLibrary>
#include <QCoreApplication>
#endif

AutoUpdateChecker::AutoUpdateChecker(QObject *parent) :
    QObject(parent)
{
    createNetworkManager();

    const QString currentVersion(VERSION_STR);
    qDebug() << "Current Moonlight version:" << currentVersion;
#ifdef Q_OS_WIN
    const bool windowsProduct = true;
#else
    const bool windowsProduct = false;
#endif
    if (!parseVersion(currentVersion, m_CurrentVersionQuad, windowsProduct)) {
        // Development hashes and vrr-lite builds have no ordered release version.
        qDebug() << "Skipping update checks for an unversioned development build";
    }
#ifdef Q_OS_WIN
    // VERSION_STR is a display identity. Historical packages and local rebuilds
    // can share that label while carrying different Windows product versions.
    if (!m_CurrentVersionQuad.isEmpty()) m_CurrentVersionQuad = installedWindowsVersion();
#endif
}

QVector<int> AutoUpdateChecker::installedWindowsVersion()
{
#ifdef Q_OS_WIN
    QLibrary library(QStringLiteral("version"));
    using Size = DWORD (WINAPI *)(LPCWSTR, LPDWORD);
    using Read = BOOL (WINAPI *)(LPCWSTR, DWORD, DWORD, LPVOID);
    using Query = BOOL (WINAPI *)(LPCVOID, LPCWSTR, LPVOID*, PUINT);
    const auto size = reinterpret_cast<Size>(library.resolve("GetFileVersionInfoSizeW"));
    const auto read = reinterpret_cast<Read>(library.resolve("GetFileVersionInfoW"));
    const auto query = reinterpret_cast<Query>(library.resolve("VerQueryValueW"));
    if (!size || !read || !query) return {};
    const auto path = QCoreApplication::applicationFilePath();
    DWORD ignored = 0;
    const DWORD length = size(reinterpret_cast<LPCWSTR>(path.utf16()), &ignored);
    if (!length || length > 1024 * 1024) return {};
    QByteArray data(int(length), '\0');
    if (!read(reinterpret_cast<LPCWSTR>(path.utf16()), 0, length, data.data())) return {};
    VS_FIXEDFILEINFO* info = nullptr;
    UINT infoSize = 0;
    if (!query(data.constData(), L"\\", reinterpret_cast<LPVOID*>(&info), &infoSize) ||
            !info || infoSize < sizeof(VS_FIXEDFILEINFO) || info->dwSignature != 0xfeef04bd) return {};
    return {int(HIWORD(info->dwProductVersionMS)), int(LOWORD(info->dwProductVersionMS)),
            int(HIWORD(info->dwProductVersionLS)), int(LOWORD(info->dwProductVersionLS)), 0};
#else
    return {};
#endif
}

void AutoUpdateChecker::createNetworkManager()
{
    m_Nam = new QNetworkAccessManager(this);

    // Never communicate over HTTP
    m_Nam->setStrictTransportSecurityEnabled(true);
    m_Nam->setRedirectPolicy(QNetworkRequest::NoLessSafeRedirectPolicy);

    connect(m_Nam, &QNetworkAccessManager::finished,
            this, &AutoUpdateChecker::handleUpdateCheckRequestFinished);

}

void AutoUpdateChecker::stopCheck()
{
    m_Checking = false;
    if (m_Nam) m_Nam->deleteLater();
    m_Nam = nullptr;
}

void AutoUpdateChecker::start()
{
    if (m_Checking || m_CurrentVersionQuad.isEmpty()) {
        return;
    }

#if defined(Q_OS_WIN32) || defined(Q_OS_DARWIN) || defined(STEAM_LINK) || defined(APP_IMAGE) // Only run update checker on platforms without auto-update
    if (!m_Nam) createNetworkManager();
#if QT_VERSION >= QT_VERSION_CHECK(5, 14, 0) && QT_VERSION < QT_VERSION_CHECK(5, 15, 1) && !defined(QT_NO_BEARERMANAGEMENT)
    // HACK: Set network accessibility to work around QTBUG-80947 (introduced in Qt 5.14.0 and fixed in Qt 5.15.1)
    QT_WARNING_PUSH
    QT_WARNING_DISABLE_DEPRECATED
    m_Nam->setNetworkAccessible(QNetworkAccessManager::Accessible);
    QT_WARNING_POP
#endif

    m_Checking = true;
    m_Page = 1;
    m_Releases = {};
    m_Candidate = {};
    requestPage();
#endif
}

bool AutoUpdateChecker::notifyPrereleases() const
{
    return QSettings().value("notifyPrereleases", true).toBool();
}

void AutoUpdateChecker::setNotifyPrereleases(bool enabled)
{
    QSettings settings;
    settings.setValue("notifyPrereleases", enabled);
    settings.sync();
    emit notifyPrereleasesChanged();
}

void AutoUpdateChecker::dismissUpdate(const QString& version)
{
    QSettings settings;
    auto dismissed = settings.value("dismissedUpdates").toStringList();
    if (!dismissed.contains(version)) dismissed.append(version);
    settings.setValue("dismissedUpdates", dismissed);
    settings.sync();
}

void AutoUpdateChecker::requestPage()
{
    // The list includes published prereleases, which are used for VRR releases.
    // Never consult the upstream manifest or fall back to upstream releases.
    QNetworkRequest request(QUrl(QStringLiteral("https://api.github.com/repos/Nonary/moonlight-qt/releases?per_page=100&page=%1").arg(m_Page)));
    request.setRawHeader("Accept", "application/vnd.github+json");
    request.setRawHeader("User-Agent", "Moonlight-Nonary-UpdateChecker");
#if QT_VERSION >= QT_VERSION_CHECK(5, 15, 0)
    request.setAttribute(QNetworkRequest::Http2AllowedAttribute, true);
#else
    request.setAttribute(QNetworkRequest::HTTP2AllowedAttribute, true);
#endif
#if QT_VERSION >= QT_VERSION_CHECK(5, 15, 0)
    request.setTransferTimeout(20000);
#endif
    m_Nam->get(request);
}

bool AutoUpdateChecker::parseVersion(const QString& string, QVector<int>& version, bool windowsProduct)
{
    // Use the fork's PE/MSI product identity for release ordering. Daily B and
    // manual VRR mappings must compare against stable builds in one version space.
    static const QRegularExpression pattern(QStringLiteral("\\Av?([0-9]+)\\.([0-9]+)\\.([0-9]+)(?:-(vrr|daily\\.)([0-9]+)(?:\\.([0-9]+))?)?\\z"));
    const auto match = pattern.match(string);
    version.clear();
    // Audited legacy packages shipped stock 6.1.0 resources. Installed rebuilds
    // are still identified from their actual executable resources above.
    if (string == "6.1.0-vrr" || string == "v6.1.0-vrr" ||
            string == "6.1.0-vrr9.1.1" || string == "v6.1.0-vrr9.1.1") {
        version = {6, 1, 0, 0, 0};
        return true;
    }
    if (!match.hasMatch()) {
        return false;
    }
    for (int i : {1, 2, 3, 5, 6}) {
        bool ok = true;
        const int component = match.captured(i).isEmpty() ? 0 : match.captured(i).toInt(&ok);
        if (!ok) { version.clear(); return false; }
        version.append(component);
    }
    if (match.captured(4) == "daily." && !match.captured(6).isEmpty()) { version.clear(); return false; }
    if (match.captured(4) == "vrr" || match.captured(4) == "daily.") {
        // Both nightly app families share the monotonically increasing MSI/PE
        // 6.2.B sequence. Non-Windows ordering retains the displayed app base.
        // A stock 6.2.0 is older than an already installed 6.2.B fork build.
        const bool daily = match.captured(4) == "daily.";
        if (version[0] != 6 || version[2] != 0 ||
                (daily ? (version[1] != 1 && version[1] != 2) : version[1] != 1)) {
            version.clear(); return false;
        }
        int build = version[3];
        if (daily && (build < 1 || build > 65535)) { version.clear(); return false; }
        if (match.captured(4) == "vrr" && (build >= 18 || !match.captured(6).isEmpty())) {
            if (build > (std::numeric_limits<int>::max() - version[4]) / 10) { version.clear(); return false; }
            build = build * 10 + version[4];
        }
        if (windowsProduct) version = {6, 2, build, 0, 0};
        else version = {6, version[1], 0, build, 0};
    }
    return true;
}

QString AutoUpdateChecker::getPlatform()
{
#if defined(STEAM_LINK)
    return QStringLiteral("steamlink");
#elif defined(APP_IMAGE)
    return QStringLiteral("appimage");
#elif defined(Q_OS_DARWIN)
    return QStringLiteral("osx");
#else
    return QSysInfo::productType();
#endif
}

int AutoUpdateChecker::compareVersion(const QVector<int>& version1, const QVector<int>& version2)
{
    for (int i = 0; i < qMax(version1.size(), version2.size()); ++i) {
        const int v1 = version1.value(i);
        const int v2 = version2.value(i);
        if (v1 != v2) {
            return v1 < v2 ? -1 : 1;
        }
    }
    return 0;
}

QJsonObject AutoUpdateChecker::apiNightlyMetadata(const QJsonObject& release)
{
    const auto body = release.value("body").toString();
    const QString marker = QStringLiteral("<!-- nightly-identity: ");
    const auto begin = body.indexOf(marker);
    if (begin < 0 || (begin > 0 && body[begin - 1] != QChar('\n')) ||
            body.indexOf(marker, begin + marker.size()) >= 0) return {};
    const auto end = body.indexOf(QStringLiteral(" -->"), begin + marker.size());
    if (end < 0) return {};
    const auto document = QJsonDocument::fromJson(body.mid(begin + marker.size(), end - begin - marker.size()).toUtf8());
    if (!document.isObject() || !validNightlyMetadata(release, document.object())) return {};
    return document.object();
}

QJsonObject AutoUpdateChecker::findUpdate(const QJsonArray& releases, const QVector<int>& currentVersion,
                                        const QString& platform, const QString& architecture, bool allowPrereleases)
{
    if (currentVersion.isEmpty()) {
        return {};
    }

    QJsonObject latest;
    QVector<int> latestVersion = currentVersion;
    for (const auto& entry : releases) {
        const auto release = entry.toObject();
        if (!release.value("draft").isBool() || release.value("draft").toBool() ||
                !release.value("prerelease").isBool() || (release.value("prerelease").toBool() && !allowPrereleases) ||
                !release.value("tag_name").isString() || !release.value("assets").isArray()) {
            continue;
        }

        const QString tag = release.value("tag_name").toString();
        QVector<int> version;
        if (!parseVersion(tag, version, platform == "windows") || compareVersion(version, latestVersion) <= 0) {
            continue;
        }

        const QString versionString = tag.startsWith('v') ? tag.mid(1) : tag;
        QString assetName;
        if (platform == "windows") {
            const QString arch = architecture == "x86_64" ? QStringLiteral("x64") :
                                 architecture == "i386" ? QStringLiteral("x86") : architecture;
            assetName = QStringLiteral("MoonlightPortable-%1-%2.zip").arg(arch, versionString);
            // The nightly Burn setup includes ARM64; the portable ZIP is x64 only.
            if (tag.contains("-daily.") && architecture == "arm64")
                assetName = QStringLiteral("MoonlightSetup-%1.exe").arg(versionString);
        }
        else if (platform == "appimage") {
            // linuxdeploy names the image after uname -m, so Qt's arm64 is aarch64 there.
            const QString arch = architecture == "arm64" ? QStringLiteral("aarch64") : architecture;
            assetName = QStringLiteral("Moonlight-%1-%2.AppImage").arg(versionString, arch);
        }
        else if (platform == "osx") {
            assetName = QStringLiteral("Moonlight-%1.dmg").arg(versionString);
        }
        else if (platform == "steamlink") {
            assetName = QStringLiteral("Moonlight-SteamLink-%1.zip").arg(versionString);
        }
        else {
            continue;
        }

        if (tag.contains("-daily.")) {
            // Nightlies become visible only after all required package assets
            // have finished uploading; metadata is checked before announcing.
            QStringList required{"SHA256SUMS"};
            if (apiNightlyMetadata(release).isEmpty()) continue;
            if (platform == "windows") {
                required.append(QStringLiteral("MoonlightSetup-%1.exe").arg(versionString));
                required.append(QStringLiteral("MoonlightPortable-x64-%1.zip").arg(versionString));
            }
            for (const auto& entry : release.value("assets").toArray()) {
                const auto asset = entry.toObject();
                if (asset.value("state").toString() == "uploaded" && asset.value("size").toDouble() > 0)
                    required.removeAll(asset.value("name").toString());
            }
            if (!release.value("prerelease").toBool() || !required.isEmpty()) continue;
        }

        for (const auto& assetEntry : release.value("assets").toArray()) {
            const auto asset = assetEntry.toObject();
            if (asset.value("name").toString() == assetName &&
                    asset.value("state").toString() == "uploaded" && asset.value("size").toDouble() > 0) {
                if (platform == "windows" && tag.contains("-vrr")) {
                    // These public packages were inspected without installation:
                    // vrr17=6.2.17, vrr17.1=6.2.171, vrr18=6.2.180.
                    // Unknown/replaced manual assets fail closed until audited.
                    QString digest;
                    if (tag == "v6.1.0-vrr17") digest = "sha256:afd68dbb6c4d65d2804806c99dd8643325ac5003d2af180173e9597b7f90c14b";
                    else if (tag == "v6.1.0-vrr17.1") digest = "sha256:7cf6a4700676c3ee6597a519016374e6176fb8936de1c591eddca9dcd71c53c7";
                    else if (tag == "v6.1.0-vrr18") digest = "sha256:900bf4e71bd8e4ff0fc7a01cd7ec08f073adc5da03a28d5968356b5dc72c795b";
                    if (architecture != "x86_64" || digest.isEmpty() || asset.value("digest").toString() != digest) continue;
                }
                latest = release;
                latestVersion = version;
                break;
            }
        }
    }
    return latest;
}

bool AutoUpdateChecker::validNightlyMetadata(const QJsonObject& release, const QJsonObject& metadata)
{
    const auto tag = release.value("tag_name").toString();
    const auto version = tag.startsWith('v') ? tag.mid(1) : tag;
    QVector<int> parsed;
    static const QRegularExpression sha(QStringLiteral("\\A[0-9a-f]{40}\\z"));
    return parseVersion(version, parsed) && tag.contains("-daily.") &&
            metadata.value("schema_version").toInt() == 1 &&
            metadata.value("channel").toString() == "nightly" &&
            metadata.value("tag").toString() == tag &&
            metadata.value("version").toString() == version &&
            metadata.value("windows_product_version").toString() == QStringLiteral("6.2.%1").arg(parsed[2]) &&
            metadata.value("nightly_sequence").toDouble(-1) == parsed[2] &&
            sha.match(metadata.value("source_commit").toString()).hasMatch();
}

void AutoUpdateChecker::announce(const QJsonObject& update)
{
    const QString tag = update.value("tag_name").toString();
    const QString version = tag.startsWith('v') ? tag.mid(1) : tag;
    if (!notifyPrereleases() && update.value("prerelease").toBool()) return;
    if (QSettings().value("dismissedUpdates").toStringList().contains(version)) return;
    emit onUpdateAvailable(version, QStringLiteral("https://github.com/Nonary/moonlight-qt/releases/tag/") + tag);
}

void AutoUpdateChecker::finishSelection()
{
    m_Candidate = findUpdate(m_Releases, m_CurrentVersionQuad, getPlatform(), QSysInfo::buildCpuArchitecture(), notifyPrereleases());
    if (m_Candidate.isEmpty()) { stopCheck(); return; }
    announce(m_Candidate);
    stopCheck();
}

void AutoUpdateChecker::handleUpdateCheckRequestFinished(QNetworkReply* reply)
{
    Q_ASSERT(reply->isFinished());
    reply->deleteLater();
    if (reply->error() != QNetworkReply::NoError) {
        stopCheck();
        qWarning() << "Update checking failed with error:" << reply->error();
        return;
    }
    QJsonParseError error;
    const auto document = QJsonDocument::fromJson(reply->readAll(), &error);
    if (!document.isArray()) { stopCheck(); return; }
    for (const auto& release : document.array()) m_Releases.append(release);
    if (document.array().size() == 100) {
        if (++m_Page > 10) { stopCheck(); return; }
        requestPage();
        return;
    }
    finishSelection();
}
