#pragma once

// Startup probing must not enable VRR presentation, but Linux VRR playback
// prefers Vulkan and therefore requests full-range video. The probe has to
// carry the same renderer preference or the host color-range request can
// disagree with the playback frontend.
inline bool decoderPrefersVrrCapableRenderer(bool enableVrr, bool preferVrrRenderer)
{
    return enableVrr || preferVrrRenderer;
}

// These match COLOR_RANGE_LIMITED / COLOR_RANGE_FULL in Limelight.h.
enum {
    kNegotiatedColorRangeLimited = 0,
    kNegotiatedColorRangeFull = 1
};

// AMF AV1 may omit full-range bitstream metadata. Force libplacebo to treat
// mapped frames as full range only when that is what the host was asked to
// encode. A later Vulkan frontend must not reinterpret a limited-range stream.
inline bool vulkanShouldForceMappedFullRange(int negotiatedColorRange)
{
    return negotiatedColorRange == kNegotiatedColorRangeFull;
}
