#pragma once

#include <QStringList>
#include <functional>

// A connection owns only a setting that it actually changed. Never disable
// composition that was already forced by Steam or another application.
class GamescopeComposition {
public:
    using Command = std::function<bool(const QStringList&, QString&)>;
    explicit GamescopeComposition(Command command = runCommand);
    ~GamescopeComposition();
    GamescopeComposition(const GamescopeComposition&) = delete;
    GamescopeComposition& operator=(const GamescopeComposition&) = delete;
    bool begin(bool enabled, bool gamingMode);
    bool restore();
    QString error() const { return m_Error; }

private:
    static bool runCommand(const QStringList& arguments, QString& output);
    bool read(bool& value);
    bool command(const QStringList& arguments);
    Command m_Command;
    QString m_Error;
    bool m_Changed = false;
};
