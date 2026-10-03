#include "gamescopecomposition.h"

#include <QDebug>
#include <QProcess>
#include <QRegularExpression>
#include <QStandardPaths>
#include <utility>

GamescopeComposition::GamescopeComposition(Command command)
    : m_Command(std::move(command))
{
}

GamescopeComposition::~GamescopeComposition()
{
    restore();
}

bool GamescopeComposition::runCommand(const QStringList& arguments, QString& output)
{
    QString executable = QStandardPaths::findExecutable("gamescopectl");
    QStringList args = arguments;
    if (executable.isEmpty()) {
        // moonlight-dev runs in Distrobox. Use its installed host bridge,
        // without installing helpers or relying on the host's stale session
        // environment. Arguments are passed directly, never through a shell.
        executable = QStandardPaths::findExecutable("distrobox-host-exec");
        if (executable.isEmpty() || QStandardPaths::findExecutable("host-spawn").isEmpty()) {
            output = "gamescopectl and an installed host bridge are unavailable";
            return false;
        }
        args = {"env", "GAMESCOPE_WAYLAND_DISPLAY=" + qEnvironmentVariable("GAMESCOPE_WAYLAND_DISPLAY"),
                "XDG_RUNTIME_DIR=" + qEnvironmentVariable("XDG_RUNTIME_DIR"), "gamescopectl"};
        args.append(arguments);
    }
    QProcess process;
    process.setProcessChannelMode(QProcess::MergedChannels);
    process.start(executable, args);
    if (!process.waitForStarted(3000) || !process.waitForFinished(5000)) {
        output = process.errorString();
        process.kill();
        process.waitForFinished(1000);
        return false;
    }
    output = QString::fromUtf8(process.readAll());
    return process.exitStatus() == QProcess::NormalExit && process.exitCode() == 0 &&
        !output.contains("Command not found", Qt::CaseInsensitive);
}

bool GamescopeComposition::command(const QStringList& arguments)
{
    QString output;
    if (!m_Command(arguments, output)) {
        m_Error = output.trimmed();
        qWarning().noquote() << "Gamescope composition control failed:" << m_Error;
        return false;
    }
    return true;
}

bool GamescopeComposition::read(bool& value)
{
    QString output;
    if (!m_Command({"composite_force"}, output)) {
        m_Error = output.trimmed();
        return false;
    }
    const auto match = QRegularExpression("\\bcomposite_force:\\s*(true|false|0|1)\\b",
                         QRegularExpression::CaseInsensitiveOption).match(output);
    if (!match.hasMatch()) {
        m_Error = "Gamescope did not report a readable composite_force value";
        return false;
    }
    value = match.captured(1).compare("true", Qt::CaseInsensitive) == 0 || match.captured(1) == "1";
    return true;
}

bool GamescopeComposition::begin(bool enabled, bool gamingMode)
{
    if (!enabled || !gamingMode) return true;
    bool original = false;
    if (!read(original)) return false;
    if (original) {
        qInfo("Gamescope composition test: already forced; preserving original setting");
        return true;
    }
    // A command may apply before failing or timing out. Restore even then.
    m_Changed = true;
    bool current = false;
    if (!command({"composite_force", "1"}) || !read(current) || !current) {
        if (m_Error.isEmpty()) m_Error = "Gamescope did not retain forced composition";
        const auto failure = m_Error;
        restore();
        m_Error = failure;
        return false;
    }
    qInfo("Gamescope composition test: forced and verified for this stream");
    return true;
}

bool GamescopeComposition::restore()
{
    if (!m_Changed) return true;
    bool current = true;
    if (!command({"composite_force", "0"}) || !read(current) || current) {
        qWarning("Gamescope composition restore failed; run gamescopectl composite_force 0 in Gaming Mode");
        return false;
    }
    m_Changed = false;
    qInfo("Gamescope composition test: restored original setting");
    return true;
}
