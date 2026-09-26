#include "networkbuffers.h"

#include <QClipboard>
#include <QFile>
#include <QGuiApplication>
#include <QProcess>
#include <QStandardPaths>

namespace {

// moonlight-common-c asks for RTP_RECV_PACKETS_BUFFERED_PYROWAVE (8192)
// packets of packetSize + MAX_RTP_HEADER_SIZE (1392 + 16) bytes.
constexpr qint64 k_RequiredBytes = 8192LL * (1392 + 16);
// Headroom above the request, so a larger packet size still fits
constexpr qint64 k_RecommendedBytes = 32LL * 1024 * 1024;

const char* const k_ConfFile = "/etc/sysctl.d/60-moonlight-pyrowave.conf";

qint64 readRmemMax()
{
#ifdef Q_OS_LINUX
    QFile file(QStringLiteral("/proc/sys/net/core/rmem_max"));
    if (file.open(QIODevice::ReadOnly)) {
        bool ok = false;
        const qint64 value = file.readAll().trimmed().toLongLong(&ok);
        if (ok) {
            return value;
        }
    }
#endif
    return 0;
}

QString rootScript()
{
    return QStringLiteral("sysctl -w net.core.rmem_max=%1 && printf 'net.core.rmem_max = %1\\n' > %2")
        .arg(k_RecommendedBytes).arg(QString::fromLatin1(k_ConfFile));
}

// pkexec runs on the host, so a containerized build reaches it through the
// container's host bridge. Flatpak builds have no such permission.
QStringList applyCommand()
{
#ifdef Q_OS_LINUX
    if (QFile::exists(QStringLiteral("/.flatpak-info"))) {
        return {};
    }
    QStringList command = { QStringLiteral("pkexec"), QStringLiteral("sh"), QStringLiteral("-c"), rootScript() };
    const bool container = QFile::exists(QStringLiteral("/run/.containerenv")) ||
                           !qEnvironmentVariableIsEmpty("CONTAINER_ID");
    if (container) {
        for (const auto& bridge : { QStringLiteral("distrobox-host-exec"), QStringLiteral("host-spawn") }) {
            if (!QStandardPaths::findExecutable(bridge).isEmpty()) {
                command.prepend(bridge);
                return command;
            }
        }
        if (!QStandardPaths::findExecutable(QStringLiteral("flatpak-spawn")).isEmpty()) {
            command.prepend(QStringLiteral("--host"));
            command.prepend(QStringLiteral("flatpak-spawn"));
            return command;
        }
        return {};
    }
    if (QStandardPaths::findExecutable(QStringLiteral("pkexec")).isEmpty()) {
        return {};
    }
    return command;
#else
    return {};
#endif
}

}

NetworkBuffers::NetworkBuffers(QObject* parent)
    : QObject(parent), m_ApplyCommand(applyCommand())
{
    refresh();
}

bool NetworkBuffers::receiveBufferTooSmall()
{
    const qint64 current = readRmemMax();
    return current > 0 && current < k_RequiredBytes;
}

int NetworkBuffers::recommendedMb() const
{
    return int(k_RecommendedBytes / (1024 * 1024));
}

QString NetworkBuffers::manualCommand() const
{
    return QStringLiteral("sudo sysctl -w net.core.rmem_max=%1 && echo 'net.core.rmem_max = %1' | sudo tee %2")
        .arg(k_RecommendedBytes).arg(QString::fromLatin1(k_ConfFile));
}

void NetworkBuffers::refresh()
{
    m_CurrentBytes = readRmemMax();
    m_NeedsFix = m_CurrentBytes > 0 && m_CurrentBytes < k_RequiredBytes;
    emit changed();
}

void NetworkBuffers::apply()
{
    if (m_Busy || m_ApplyCommand.isEmpty()) {
        return;
    }

    m_Busy = true;
    m_Message = tr("Waiting for authorization…");
    emit changed();

    auto process = new QProcess(this);
    connect(process, &QProcess::finished, this, [this, process](int exitCode, QProcess::ExitStatus status) {
        const QString error = QString::fromUtf8(process->readAllStandardError()).trimmed();
        process->deleteLater();
        m_Busy = false;
        refresh();
        if (status == QProcess::NormalExit && exitCode == 0 && !m_NeedsFix) {
            m_Message = tr("Receive buffer limit raised to %1 MB and saved for future boots. Reconnect any running stream.")
                            .arg(recommendedMb());
        }
        else if (exitCode == 126) {
            m_Message = tr("Authorization was cancelled.");
        }
        else if (exitCode == 127) {
            m_Message = tr("No password prompt is available here (for example in Gaming Mode). Switch to Desktop Mode, or run the command below in a terminal.");
        }
        else {
            m_Message = tr("The change failed (exit code %1). Run the command below in a terminal instead.").arg(exitCode);
            if (!error.isEmpty()) {
                m_Message += QStringLiteral("\n") + error;
            }
        }
        emit changed();
    });
    connect(process, &QProcess::errorOccurred, this, [this, process](QProcess::ProcessError error) {
        if (error != QProcess::FailedToStart) {
            return;
        }
        process->deleteLater();
        m_Busy = false;
        m_Message = tr("Could not start the authorization helper. Run the command below in a terminal instead.");
        emit changed();
    });

    const QString program = m_ApplyCommand.first();
    process->start(program, m_ApplyCommand.mid(1));
}

void NetworkBuffers::copyCommand()
{
    QGuiApplication::clipboard()->setText(manualCommand());
    m_Message = tr("Command copied. Paste it into a terminal (Konsole on Steam Deck) and enter your password.");
    emit changed();
}
