#pragma once

#include <QFile>
#include <QJsonObject>
#include <QLockFile>
#include <QString>
#include <QVector>
#include <memory>

// A session-scoped, opt-in wrapper around the existing environment-driven
// tracer. No file work is done on the video delivery thread.
class DiagnosticCapture
{
public:
    static QString rootDirectory();
    static std::unique_ptr<DiagnosticCapture> begin(const QString& root,
        const QJsonObject& metadata, QString& error);
    ~DiagnosticCapture();

    QString directory() const { return m_Directory; }
    // Call after decoder/trace shutdown and after draining the app logger.
    void finish();

    // Called only by the app's serialized logger, with already-redacted text.
    static void appendLog(const QString& message);
    static bool isActive();

    // Export only a closed capture. An interrupted capture remains exportable
    // after its process exits, and its manifest does not claim clean closure.
    static QString exportLatest(const QString& root, QString& error);

private:
    DiagnosticCapture() = default;
    bool saveManifest();
    struct EnvironmentValue {
        QByteArray name;
        QString value;
        bool present;
    };
    QString m_Directory;
    QFile m_Log;
    QJsonObject m_Metadata;
    std::unique_ptr<QLockFile> m_Lock;
    QVector<EnvironmentValue> m_Environment;
    bool m_Finished = false;
    bool m_LogWriteFailed = false;
};
