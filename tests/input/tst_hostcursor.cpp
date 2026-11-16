#include "../../app/streaming/input/hostcursormath.h"

#include <cstdio>
#include <cstdlib>
#include <vector>

using namespace HostCursorMath;

static int g_Failures;

static void require(bool condition, const char* message)
{
    if (!condition) {
        std::fprintf(stderr, "FAIL: %s\n", message);
        g_Failures++;
    }
}

// Forward mapping from mouse.cpp: the host gets (windowX - video.x) on a
// video.w-wide plane and scales it to the stream's reference width.
static int windowToStream(const Rect& video, int reference, int windowX, bool horizontal)
{
    int offset = horizontal ? video.x : video.y;
    int extent = horizontal ? video.w : video.h;
    return (int)std::lround((double)(windowX - offset) * reference / extent);
}

static void testStreamToWindowInvertsAbsoluteInput()
{
    // 1920x1080 stream pillarboxed in a 2560x1440 window: no bars, 4:3 scale.
    const Rect fullscreen { 0, 0, 2560, 1440 };
    Point p = streamToWindow(fullscreen, 1920, 1080, 960, 540);
    require(p.x == 1280 && p.y == 720, "centre of a scaled stream");

    // 1920x1080 stream letterboxed in a 1000x1000 window: 1000x563 video, 219 px bars.
    const Rect letterboxed { 0, 219, 1000, 563 };
    p = streamToWindow(letterboxed, 1920, 1080, 0, 0);
    require(p.x == 0 && p.y == 219, "letterbox top-left");
    p = streamToWindow(letterboxed, 1920, 1080, 1920, 1080);
    require(p.x == 1000 && p.y == 782, "letterbox bottom-right");

    for (int sx = 0; sx <= 1920; sx += 7) {
        Point w = streamToWindow(letterboxed, 1920, 1080, sx, 540);
        // A window pixel spans ~1.9 stream pixels, so the round trip is within one window pixel.
        int back = windowToStream(letterboxed, 1920, w.x, true);
        require(std::abs(back - sx) <= 2, "round trip through the window");
    }

    p = streamToWindow(letterboxed, 0, 1080, 10, 10);
    require(p.x == 0 && p.y == 0, "zero reference is rejected");
}

static void testClipRectIsClampedToTheVideo()
{
    const Rect video { 100, 50, 960, 540 };  // 1920x1080 stream at half size
    Rect r = streamRectToWindow(video, 1920, 1080, 200, 100, 1000, 700);
    require(r.x == 200 && r.y == 100 && r.w == 400 && r.h == 300, "clip maps to window units");

    r = streamRectToWindow(video, 1920, 1080, -500, -500, 5000, 5000);
    require(r == video, "clip larger than the stream is the video");

    r = streamRectToWindow(video, 1920, 1080, 3000, 3000, 4000, 4000);
    require(r.w == 0 && r.h == 0, "clip outside the stream is empty");
}

static void testCursorScale()
{
    const Rect video { 0, 0, 2560, 1440 };
    // Host stream at 0.75 stream px per desktop px, shown at 4/3: the cursor keeps its host size.
    double s = cursorScale(49152, video, 1920);
    require(std::abs(s - 1.0) < 1e-9, "scale composes host and window factors");
    require(cursorScale(0x10000, video, 0) == 1.0, "zero reference falls back to 1");
    require(scaledDimension(32, 1.5) == 48, "scaled dimension");
    require(scaledDimension(256, 10.0) == kMaxCursorDimension, "scaled dimension is capped");
    require(scaledDimension(4, 0.01) == 1, "scaled dimension is at least 1");
}

static void testScaleBgra()
{
    // Identity keeps the pixels.
    std::vector<uint8_t> src { 1, 2, 3, 4, 5, 6, 7, 8 };
    std::vector<uint8_t> out = scaleBgra(src.data(), 2, 1, 2, 1);
    require(out == src, "identity scale");

    // Opaque red next to fully transparent black: interpolating straight alpha would
    // darken the edge; premultiplied interpolation keeps it red.
    std::vector<uint8_t> edge { 0, 0, 255, 255, 0, 0, 0, 0 };
    out = scaleBgra(edge.data(), 2, 1, 4, 1);
    require(out.size() == 16, "upscaled size");
    for (int i = 0; i < 4; i++) {
        const uint8_t* p = &out[i * 4];
        if (p[3] != 0) {
            require(p[2] == 255 && p[1] == 0 && p[0] == 0, "edge colour is not darkened");
        }
    }
    require(out[3] == 255 && out[15] == 0, "edge alpha ends stay opaque and transparent");
    require(out[7] > out[11], "alpha ramps down across the edge");

    // Downscaling a uniform image is exact.
    std::vector<uint8_t> uniform(8 * 8 * 4);
    for (size_t i = 0; i < uniform.size(); i += 4) {
        uniform[i] = 10; uniform[i + 1] = 20; uniform[i + 2] = 30; uniform[i + 3] = 200;
    }
    out = scaleBgra(uniform.data(), 8, 8, 3, 3);
    bool exact = true;
    for (size_t i = 0; i < out.size(); i += 4) {
        exact &= out[i] == 10 && out[i + 1] == 20 && out[i + 2] == 30 && out[i + 3] == 200;
    }
    require(exact, "uniform image survives downscaling");

    out = scaleBgra(uniform.data(), 0, 8, 3, 3);
    require(out.size() == 36, "invalid source yields a transparent image of the requested size");
}

int main()
{
    testStreamToWindowInvertsAbsoluteInput();
    testClipRectIsClampedToTheVideo();
    testCursorScale();
    testScaleBgra();
    if (g_Failures == 0) {
        std::printf("PASS host cursor math\n");
    }
    return g_Failures == 0 ? 0 : 1;
}
