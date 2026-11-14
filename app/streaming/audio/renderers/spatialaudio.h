#pragma once

#include "renderer.h"

#include <QList>
#include <QString>
#include <memory>

// Opt-in Windows output for decoded 5.1 or 7.1 PCM. An empty ID selects the current
// default console render endpoint at initialization; a nonempty ID pins that
// endpoint. Both paths require HDMI/DisplayPort and never change audio policy.
class SpatialAudioRenderer : public IAudioRenderer
{
public:
    struct EndpointInfo {
        QString id;
        QString name;
        bool isDefault = false;
    };

    // Read-only inventory of active HDMI/DisplayPort render endpoints. This
    // does not activate Spatial Audio, open a stream, or prove provider support.
    // Empty result + empty error means no matching endpoints; on failure the
    // result is empty and error (if supplied) explains the failure.
    static QList<EndpointInfo> enumerateEndpoints(QString* error = nullptr);

    explicit SpatialAudioRenderer(const QString& endpointId = {});
    ~SpatialAudioRenderer() override;

    bool prepareForPlayback(const OPUS_MULTISTREAM_CONFIGURATION* opusConfig) override;
    void* getAudioBuffer(int* size) override;
    bool submitAudio(int bytesWritten) override;
    AudioFormat getAudioBufferFormat() override;

private:
    struct Impl;
    std::unique_ptr<Impl> m_Impl;
};
