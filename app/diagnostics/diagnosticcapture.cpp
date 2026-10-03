#include "diagnosticcapture.h"
#include "diagnosticzip.h"

#include <QCoreApplication>
#include <QCryptographicHash>
#include <QDateTime>
#include <QDir>
#include <QFileInfo>
#include <QJsonDocument>
#include <QMutex>
#include <QSaveFile>
#include <QStandardPaths>
#include <QSysInfo>
#include <QUuid>
#include <atomic>
#ifdef Q_OS_WIN
#include <Windows.h>
#include <cstdlib>
#endif

namespace {
QMutex captureMutex;
DiagnosticCapture* activeCapture = nullptr;
std::atomic_bool captureActive{false};

bool setEnvironment(const QByteArray& name, const QString& value, bool present = true)
{
#ifdef Q_OS_WIN
    // Use the wide CRT environment so a Windows profile path need not be
    // representable in the active ANSI code page. Qt reads the wide value too.
    return _wputenv_s(reinterpret_cast<const wchar_t*>(QString::fromLatin1(name).utf16()),
        present ? reinterpret_cast<const wchar_t*>(value.utf16()) : L"") == 0;
#else
    return present ? qputenv(name.constData(), value.toLocal8Bit()) : qunsetenv(name.constData());
#endif
}

bool localDirectory(const QString& path)
{
    if (path.startsWith("\\\\") || path.startsWith("//")) return false;
#ifdef Q_OS_WIN
    // Also reject mapped network drives; their spelling does not start with UNC.
    wchar_t volume[MAX_PATH];
    const auto native = QDir::toNativeSeparators(path).toStdWString();
    if (!GetVolumePathNameW(native.c_str(), volume, MAX_PATH)) return false;
    const auto type = GetDriveTypeW(volume);
    return type != DRIVE_REMOTE && type != DRIVE_UNKNOWN && type != DRIVE_NO_ROOT_DIR;
#else
    return true;
#endif
}
}

QString DiagnosticCapture::rootDirectory()
{
    const QString desktop = QStandardPaths::writableLocation(QStandardPaths::DesktopLocation);
    return desktop.isEmpty() ? QString() : QDir(desktop).filePath("vrr-diagnostics");
}

std::unique_ptr<DiagnosticCapture> DiagnosticCapture::begin(const QString& root,
    const QJsonObject& metadata, QString& error)
{
    error.clear();
    QMutexLocker guard(&captureMutex);
    if (activeCapture) {
        error = QCoreApplication::translate("DiagnosticCapture", "A diagnostic capture is already running.");
        return {};
    }
    // Existing launchers remain authoritative. Never silently redirect their trace.
    if (!qEnvironmentVariableIsEmpty("MOONLIGHT_VRR_TRACE")) {
        error = QCoreApplication::translate("DiagnosticCapture",
            "An external tracing launcher is active. Its trace destination was preserved; launch Moonlight normally to use Settings diagnostics.");
        return {};
    }
    if (root.isEmpty() || !QFileInfo(root).isAbsolute() || !localDirectory(root) || !QDir().mkpath(root) ||
            !localDirectory(QFileInfo(root).canonicalFilePath())) {
        error = QCoreApplication::translate("DiagnosticCapture", "Cannot create a local diagnostics folder.");
        return {};
    }
    auto capture = std::unique_ptr<DiagnosticCapture>(new DiagnosticCapture);
    const auto id = QDateTime::currentDateTimeUtc().toString("yyyyMMdd-HHmmss-zzz") +
        "-" + QUuid::createUuid().toString(QUuid::WithoutBraces);
    capture->m_Directory = QDir(root).filePath(id);
    if (!QDir(root).mkdir(id)) {
        error = QCoreApplication::translate("DiagnosticCapture", "Cannot create the diagnostic capture folder.");
        return {};
    }
    capture->m_Lock = std::make_unique<QLockFile>(QDir(capture->m_Directory).filePath("capture.lock"));
    capture->m_Lock->setStaleLockTime(0);
    capture->m_Log.setFileName(QDir(capture->m_Directory).filePath("Moonlight.log"));
    if (!capture->m_Lock->tryLock(0) || !capture->m_Log.open(QIODevice::WriteOnly | QIODevice::NewOnly)) {
        error = QCoreApplication::translate("DiagnosticCapture", "Cannot open the diagnostic session log.");
        return {};
    }
    capture->m_Metadata = metadata;
    capture->m_Metadata["capture_schema"] = 1;
    capture->m_Metadata["started_utc"] = QDateTime::currentDateTimeUtc().toString(Qt::ISODateWithMs);
    capture->m_Metadata["application_version"] = QCoreApplication::applicationVersion();
    capture->m_Metadata["os"] = QSysInfo::prettyProductName();
    capture->m_Metadata["architecture"] = QSysInfo::currentCpuArchitecture();
    capture->m_Metadata["clean_session_close"] = false;
    capture->m_Metadata["deep_trace"] = true;
    capture->m_Metadata["alignment_override"] = qEnvironmentVariable("MOONLIGHT_VRR_ALIGN");
    capture->m_Metadata["separate_devices_override"] = qEnvironmentVariable("D3D11VA_FORCE_SEPARATE_DEVICES");
    capture->m_Metadata["composition_override"] = qEnvironmentVariable("MOONLIGHT_VRR_COMPOSITION");
    QFile executable(QCoreApplication::applicationFilePath());
    QCryptographicHash hash(QCryptographicHash::Sha256);
    if (executable.open(QIODevice::ReadOnly) && hash.addData(&executable))
        capture->m_Metadata["executable_sha256"] = QString::fromLatin1(hash.result().toHex());
    if (!capture->saveManifest()) {
        error = QCoreApplication::translate("DiagnosticCapture", "Cannot write the diagnostic capture manifest.");
        return {};
    }
    // Runs may start within the same clock millisecond. An atomic pointer
    // avoids guessing which run is latest from their random IDs or ZIP mtimes.
    // Require this before enabling capture: a stale pointer must never make
    // "Export latest" silently export an older run.
    QSaveFile latest(QDir(root).filePath("latest-capture.json"));
    const auto latestBytes = QJsonDocument(QJsonObject{{"directory", id}}).toJson();
    if (!latest.open(QIODevice::WriteOnly) || latest.write(latestBytes) != latestBytes.size() || !latest.commit()) {
        error = QCoreApplication::translate("DiagnosticCapture", "Cannot register the latest diagnostic capture.");
        return {};
    }
    const QVector<QPair<QByteArray, QString>> values = {
        {"MOONLIGHT_VRR_TRACE", QDir(capture->m_Directory).filePath("Moonlight.vrrtrace")},
        {"MOONLIGHT_VRR_DEEP_TRACE", "1"}
    };
    for (const auto& value : values) {
        capture->m_Environment.append({value.first, qEnvironmentVariable(value.first.constData()),
                                      qEnvironmentVariableIsSet(value.first.constData())});
        if (!setEnvironment(value.first, value.second)) {
            // Restore under this same lock; destruction below must not try to
            // finish a capture which was never published to the logger.
            for (const auto& prior : capture->m_Environment)
                setEnvironment(prior.name, prior.value, prior.present);
            capture->m_Environment.clear();
            error = QCoreApplication::translate("DiagnosticCapture", "Cannot enable the diagnostic capture environment.");
            return {};
        }
    }
    activeCapture = capture.get();
    captureActive.store(true);
    return capture;
}

DiagnosticCapture::~DiagnosticCapture()
{
    // Failed begin() never publishes an environment and holds captureMutex.
    if (!m_Environment.isEmpty()) finish();
}

bool DiagnosticCapture::saveManifest()
{
    QSaveFile file(QDir(m_Directory).filePath("capture-info.json"));
    const auto bytes = QJsonDocument(m_Metadata).toJson();
    return file.open(QIODevice::WriteOnly) && file.write(bytes) == bytes.size() && file.commit();
}

void DiagnosticCapture::appendLog(const QString& message)
{
    if (!isActive()) return;
    QMutexLocker guard(&captureMutex);
    if (!activeCapture || activeCapture->m_LogWriteFailed) return;
    // Per-capture text logs are bounded independently of the process log.
    if (activeCapture->m_Log.size() >= 10 * 1024 * 1024) {
        activeCapture->m_Metadata["session_log_size_capped"] = true;
        return;
    }
    const auto bytes = message.toUtf8();
    if (activeCapture->m_Log.write(bytes) != bytes.size() || !activeCapture->m_Log.flush())
        activeCapture->m_LogWriteFailed = true;
}

bool DiagnosticCapture::isActive()
{
    return captureActive.load();
}

void DiagnosticCapture::finish()
{
    QMutexLocker guard(&captureMutex);
    if (m_Finished) return;
    if (activeCapture == this) {
        captureActive.store(false);
        activeCapture = nullptr;
    }
    if (m_Log.isOpen() && !m_Log.flush()) m_LogWriteFailed = true;
    m_Log.close();
    m_Metadata["session_log_write_failed"] = m_LogWriteFailed;
    m_Metadata["clean_session_close"] = true;
    m_Metadata["finished_utc"] = QDateTime::currentDateTimeUtc().toString(Qt::ISODateWithMs);
    m_Metadata["trace_files"] = QDir(m_Directory).entryList({"*.vrrtrace"}, QDir::Files).size();
    // A failed final manifest remains explicitly unclosed rather than falsely
    // certifying the recording. Trace integrity still requires exact replay.
    saveManifest();
    for (const auto& prior : m_Environment) setEnvironment(prior.name, prior.value, prior.present);
    m_Environment.clear();
    m_Lock->unlock();
    m_Finished = true;
}

QString DiagnosticCapture::exportLatest(const QString& root, QString& error)
{
    error.clear();
    if (root.isEmpty() || !QFileInfo(root).isAbsolute()) {
        error = QCoreApplication::translate("DiagnosticCapture", "The diagnostics folder is unavailable.");
        return {};
    }
    const QDir directory(root);
    auto captures = directory.entryInfoList(QDir::Dirs | QDir::NoDotAndDotDot | QDir::NoSymLinks, QDir::Time);
    QFile latest(directory.filePath("latest-capture.json"));
    if (latest.open(QIODevice::ReadOnly)) {
        const auto name = QJsonDocument::fromJson(latest.read(4096)).object()["directory"].toString();
        // Match only enumerated direct children, never a path supplied by JSON.
        for (int i = 0; i < captures.size(); ++i) {
            if (captures[i].fileName() == name) {
                captures.prepend(captures.takeAt(i));
                break;
            }
        }
    }
    for (const auto& capture : captures) {
        const QDir source(capture.absoluteFilePath());
        if (!source.exists("capture-info.json")) continue;
        QLockFile lock(source.filePath("capture.lock"));
        lock.setStaleLockTime(0);
        if (!lock.tryLock(0)) {
            error = QCoreApplication::translate("DiagnosticCapture", "The latest recording is still active. Disconnect the stream and wait for cleanup before exporting.");
            return {};
        }
        const QString destination = directory.filePath(capture.fileName() + "-" +
            QUuid::createUuid().toString(QUuid::WithoutBraces) + ".zip");
        return writeDiagnosticZip(source, destination, error) ? destination : QString();
    }
    error = QCoreApplication::translate("DiagnosticCapture", "No diagnostic recording is available yet. Enable recording and run a stream first.");
    return {};
}
