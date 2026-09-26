#pragma once

#include <QObject>
#include <QString>

// PyroWave frames arrive as hundreds of packets at line rate. On Linux the
// video socket's receive buffer is capped by net.core.rmem_max, and packets
// beyond it are dropped by the kernel. This checks the limit and raises it on
// the host with the user's authorization.
class NetworkBuffers : public QObject
{
    Q_OBJECT
    Q_PROPERTY(bool needsFix READ needsFix NOTIFY changed)
    Q_PROPERTY(bool canApply READ canApply CONSTANT)
    Q_PROPERTY(bool busy READ busy NOTIFY changed)
    Q_PROPERTY(int currentKb READ currentKb NOTIFY changed)
    Q_PROPERTY(int recommendedMb READ recommendedMb CONSTANT)
    Q_PROPERTY(QString message READ message NOTIFY changed)
    Q_PROPERTY(QString manualCommand READ manualCommand CONSTANT)

public:
    explicit NetworkBuffers(QObject* parent = nullptr);

    // Whether the kernel limit is below what a PyroWave stream asks for.
    // Returns false where the limit cannot be read or does not apply.
    static bool receiveBufferTooSmall();

    bool needsFix() const { return m_NeedsFix; }
    bool canApply() const { return !m_ApplyCommand.isEmpty(); }
    bool busy() const { return m_Busy; }
    int currentKb() const { return int(m_CurrentBytes / 1024); }
    int recommendedMb() const;
    QString message() const { return m_Message; }
    QString manualCommand() const;

    Q_INVOKABLE void refresh();
    Q_INVOKABLE void apply();
    Q_INVOKABLE void copyCommand();

signals:
    void changed();

private:
    bool m_NeedsFix = false;
    bool m_Busy = false;
    qint64 m_CurrentBytes = 0;
    QString m_Message;
    QStringList m_ApplyCommand;
};
