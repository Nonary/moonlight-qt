#pragma once

#include <QString>
#include <QSize>

#include "SDL_compat.h"
#include "settings/streamingpreferences.h"

// Persists the results of the decoder capability probes so that we don't have to
// instantiate (and tear down) GPU devices and decoders on every launch just to
// answer questions whose answers only change when the GPU, driver, display
// backend or Moonlight build changes. Each of those is part of the cache key.
//
// Set MOONLIGHT_PROBE_CACHE=0 to bypass the cache.
class DecoderProbeCache
{
public:
    struct DecoderInfo {
        bool hardwareAccelerated;
        bool fullScreenOnly;
        bool hdrSupported;
        QSize maxResolution;
    };

    static bool enabled();

    static bool lookupDecoderInfo(DecoderInfo& info);
    static void storeDecoderInfo(const DecoderInfo& info);

    // Returns true and sets 'availability' (as Session::DecoderAvailability's int value) on a hit
    static bool lookupAvailability(StreamingPreferences::VideoDecoderSelection vds,
                                   int videoFormat, int width, int height, int frameRate,
                                   int& availability);
    static void storeAvailability(StreamingPreferences::VideoDecoderSelection vds,
                                  int videoFormat, int width, int height, int frameRate,
                                  int availability);

    // What Session::populateDecoderProperties() learns from the pre-connection probe
    struct StreamProperties {
        int capabilities;
        int colorSpace;
        int colorRange;
        bool alwaysFullScreen;
    };

    static bool lookupStreamProperties(StreamingPreferences::VideoDecoderSelection vds,
                                       StreamingPreferences::RendererSelection renderer,
                                       int videoFormat, int width, int height, int frameRate,
                                       bool enableVrr, StreamProperties& props);
    static void storeStreamProperties(StreamingPreferences::VideoDecoderSelection vds,
                                      StreamingPreferences::RendererSelection renderer,
                                      int videoFormat, int width, int height, int frameRate,
                                      bool enableVrr, const StreamProperties& props);

    // Free-form per-environment verdicts (e.g. "this driver gets deprioritized")
    static bool lookupFlag(const QString& name);
    static void storeFlag(const QString& name, bool value);

private:
    static QString environmentKey();
    static QString gpuIdentity();
    static QString settingsGroup();
};
