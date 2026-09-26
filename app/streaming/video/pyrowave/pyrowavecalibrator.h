#pragma once

#include <QObject>
#include <QPointer>
#include <QThread>
#include <QVariantList>
#include <QVariantMap>

// Runs a short synthetic decode sweep without changing the stream settings.
// The estimate covers local decode capacity; it cannot measure a host or LAN.
class PyroWaveCalibrator : public QObject
{
    Q_OBJECT
    Q_PROPERTY(bool running READ running NOTIFY changed)
    Q_PROPERTY(QString message READ message NOTIFY changed)
    Q_PROPERTY(QVariantList results READ results NOTIFY changed)

public:
    explicit PyroWaveCalibrator(QObject* parent = nullptr);
    ~PyroWaveCalibrator() override;

    bool running() const { return m_Running; }
    QString message() const { return m_Message; }
    QVariantList results() const { return m_Results; }

    Q_INVOKABLE void start(int fps);

signals:
    void changed();

private:
    bool m_Running = false;
    QString m_Message;
    QVariantList m_Results;
    QPointer<QThread> m_Worker;
};
