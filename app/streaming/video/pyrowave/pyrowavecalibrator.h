#pragma once

#include <QObject>
#include <QPointer>
#include <QThread>
#include <QVariantList>
#include <QVariantMap>

#include <atomic>
#include <memory>

class ComputerManager;

// Finds, for each resolution and PyroWave format at one frame rate, the
// highest bitrate at which this device decodes and draws 99% of frames within
// the frame period, capped by a measured host-to-client download and both
// known wired link speeds. It does not change settings.
class PyroWaveCalibrator : public QObject
{
    Q_OBJECT
    Q_PROPERTY(bool running READ running NOTIFY changed)
    Q_PROPERTY(QString message READ message NOTIFY changed)
    Q_PROPERTY(QString linkSummary READ linkSummary NOTIFY changed)
    Q_PROPERTY(QVariantList results READ results NOTIFY changed)

public:
    explicit PyroWaveCalibrator(QObject* parent = nullptr);
    ~PyroWaveCalibrator() override;

    bool running() const { return m_Running; }
    QString message() const { return m_Message; }
    QString linkSummary() const { return m_LinkSummary; }
    QVariantList results() const { return m_Results; }

    // displayWidth/displayHeight size the render that each test frame goes
    // through, as a stream drawing to that display would. Zero renders at the
    // stream's own resolution.
    Q_INVOKABLE void start(ComputerManager* manager, const QString& hostUuid, int fps,
                           int displayWidth = 0, int displayHeight = 0);
    // Stops after the current network transfer or format; finished results stay.
    Q_INVOKABLE void cancel();

    // Drain the worker before replacing the client identity on a profile change.
    void stop();

signals:
    void changed();

private:
    bool m_Running = false;
    QString m_Message;
    QString m_LinkSummary;
    QVariantList m_Results;
    QPointer<QThread> m_Worker;
    std::shared_ptr<std::atomic<bool>> m_Cancel;
    quint64 m_Generation = 0;
};
