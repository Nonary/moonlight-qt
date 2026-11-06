#pragma once

// Pure helpers for the client-side cursor: mapping host stream coordinates into the
// letterboxed video region of the window, and scaling host cursor shapes. No SDL or
// Qt dependencies, so they are unit tested directly (tests/input/hostcursor.pro).

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <vector>

namespace HostCursorMath {

struct Rect {
    int x = 0;
    int y = 0;
    int w = 0;
    int h = 0;

    bool operator==(const Rect& other) const
    {
        return x == other.x && y == other.y && w == other.w && h == other.h;
    }
};

struct Point {
    int x = 0;
    int y = 0;
};

// Inverse of the absolute mouse mapping in mouse.cpp: the host receives
// (windowX - video.x) on a video.w-wide reference plane, which it scales to the
// stream's referenceWidth. `video` is the letterboxed region in window units.
inline Point streamToWindow(const Rect& video, int referenceWidth, int referenceHeight, int32_t x, int32_t y)
{
    Point p;
    if (referenceWidth <= 0 || referenceHeight <= 0) {
        return p;
    }
    // Host coordinates are int32 and may be far off the stream; keep the result in int range.
    const double limit = 1e9;
    p.x = video.x + (int)std::lround(std::clamp((double)x * video.w / referenceWidth, -limit, limit));
    p.y = video.y + (int)std::lround(std::clamp((double)y * video.h / referenceHeight, -limit, limit));
    return p;
}

// A host clip rect (exclusive right/bottom) in window units, clamped to the video.
inline Rect streamRectToWindow(const Rect& video, int referenceWidth, int referenceHeight,
                               int32_t left, int32_t top, int32_t right, int32_t bottom)
{
    Point a = streamToWindow(video, referenceWidth, referenceHeight, left, top);
    Point b = streamToWindow(video, referenceWidth, referenceHeight, right, bottom);
    Rect r;
    r.x = std::clamp(a.x, video.x, video.x + video.w);
    r.y = std::clamp(a.y, video.y, video.y + video.h);
    r.w = std::clamp(b.x, video.x, video.x + video.w) - r.x;
    r.h = std::clamp(b.y, video.y, video.y + video.h) - r.y;
    r.w = (std::max)(r.w, 0);
    r.h = (std::max)(r.h, 0);
    return r;
}

// Window units per host cursor pixel: the host's stream pixels per shape pixel
// (16.16) times the window units per stream pixel.
inline double cursorScale(uint32_t scaleQ16, const Rect& video, int referenceWidth)
{
    if (referenceWidth <= 0 || video.w <= 0 || scaleQ16 == 0) {
        return 1.0;
    }
    return (scaleQ16 / 65536.0) * video.w / referenceWidth;
}

// Largest local cursor we create, in either dimension.
constexpr int kMaxCursorDimension = 512;

inline int scaledDimension(int size, double scale)
{
    return std::clamp((int)std::lround(size * scale), 1, kMaxCursorDimension);
}

// Bilinear resample of straight-alpha BGRA, interpolated in premultiplied space so
// transparent neighbours do not darken the edges. Returns straight alpha.
inline std::vector<uint8_t> scaleBgra(const uint8_t* src, int width, int height, int outWidth, int outHeight)
{
    std::vector<uint8_t> out((size_t)outWidth * outHeight * 4);
    if (width <= 0 || height <= 0 || outWidth <= 0 || outHeight <= 0) {
        return out;
    }
    if (width == outWidth && height == outHeight) {
        std::copy(src, src + out.size(), out.begin());
        return out;
    }

    const double sx = (double)width / outWidth;
    const double sy = (double)height / outHeight;
    for (int oy = 0; oy < outHeight; oy++) {
        double fy = std::clamp((oy + 0.5) * sy - 0.5, 0.0, (double)(height - 1));
        int y0 = (int)fy;
        int y1 = (std::min)(y0 + 1, height - 1);
        double ty = fy - y0;
        for (int ox = 0; ox < outWidth; ox++) {
            double fx = std::clamp((ox + 0.5) * sx - 0.5, 0.0, (double)(width - 1));
            int x0 = (int)fx;
            int x1 = (std::min)(x0 + 1, width - 1);
            double tx = fx - x0;

            const uint8_t* p[4] = {
                &src[((size_t)y0 * width + x0) * 4], &src[((size_t)y0 * width + x1) * 4],
                &src[((size_t)y1 * width + x0) * 4], &src[((size_t)y1 * width + x1) * 4],
            };
            const double w[4] = { (1 - tx) * (1 - ty), tx * (1 - ty), (1 - tx) * ty, tx * ty };

            double premul[3] = {};
            double alpha = 0;
            for (int i = 0; i < 4; i++) {
                double a = p[i][3] / 255.0 * w[i];
                alpha += a;
                for (int c = 0; c < 3; c++) {
                    premul[c] += p[i][c] * a;
                }
            }

            uint8_t* o = &out[((size_t)oy * outWidth + ox) * 4];
            for (int c = 0; c < 3; c++) {
                o[c] = alpha > 0 ? (uint8_t)std::clamp(std::lround(premul[c] / alpha), 0L, 255L) : 0;
            }
            o[3] = (uint8_t)std::clamp(std::lround(alpha * 255), 0L, 255L);
        }
    }
    return out;
}

}
