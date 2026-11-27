#include "../../app/streaming/video/ffmpeg-renderers/d3d11composition.h"

#include <algorithm>
#include <atomic>
#include <cstdio>
#include <cwchar>
#include <thread>
#include <vector>

using Microsoft::WRL::ComPtr;

namespace {
uint64_t clockUs()
{
    static const auto frequency = [] { LARGE_INTEGER f; QueryPerformanceFrequency(&f); return f.QuadPart; }();
    static const auto epoch = [] { LARGE_INTEGER t; QueryPerformanceCounter(&t); return t.QuadPart; }();
    LARGE_INTEGER now;
    QueryPerformanceCounter(&now);
    return 1 + uint64_t(now.QuadPart - epoch) * 1000000 / frequency;
}

LRESULT CALLBACK windowProcedure(HWND window, UINT message, WPARAM wparam, LPARAM lparam)
{
    if (message == WM_CLOSE || (message == WM_KEYDOWN && wparam == VK_ESCAPE)) {
        PostQuitMessage(0);
        return 0;
    }
    return DefWindowProcW(window, message, wparam, lparam);
}

template<typename T>
T percentile(std::vector<T> values, size_t percent)
{
    if (values.empty()) return 0;
    std::sort(values.begin(), values.end());
    return values[(values.size() - 1) * percent / 100];
}

int64_t differenceUs(uint64_t time, uint64_t reference)
{
    return time >= reference ? static_cast<int64_t>(time - reference) :
        -static_cast<int64_t>(reference - time);
}

bool waitUntil(HANDLE timer, uint64_t deadline, uint64_t spinUs = 0)
{
    const auto now = clockUs();
    if (deadline > now + spinUs) {
        LARGE_INTEGER due;
        due.QuadPart = -static_cast<LONGLONG>((deadline - now - spinUs) * 10);
        if (!SetWaitableTimer(timer, &due, 0, nullptr, nullptr, FALSE) ||
                WaitForSingleObject(timer, 100) != WAIT_OBJECT_0) return false;
    }
    while (clockUs() < deadline) YieldProcessor();
    return true;
}

// An independent D3D11 device plays the role of a decoder. It writes a
// shared texture and signals the matching fence value on its GPU context.
class DecodeProducer {
public:
    ~DecodeProducer()
    {
        if (thread.joinable()) {
            stopping = true;
            SetEvent(work);
            thread.join();
        }
        if (work) CloseHandle(work);
        if (timer) CloseHandle(timer);
    }

    HRESULT initialize(ID3D11Device5* consumer, UINT width, UINT height)
    {
        ComPtr<ID3D11Device> device;
        ComPtr<ID3D11DeviceContext> context;
        ComPtr<ID3D11Device5> device5;
        HRESULT hr = D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr, 0,
            nullptr, 0, D3D11_SDK_VERSION, &device, nullptr, &context);
        if (SUCCEEDED(hr)) hr = device.As(&device5);
        if (SUCCEEDED(hr)) hr = context.As(&producerContext);
        if (SUCCEEDED(hr)) hr = device5->CreateFence(0, D3D11_FENCE_FLAG_SHARED, IID_PPV_ARGS(&producerFence));
        if (FAILED(hr)) return hr;

        HANDLE shared = nullptr;
        hr = producerFence->CreateSharedHandle(nullptr, GENERIC_ALL, nullptr, &shared);
        if (SUCCEEDED(hr)) {
            hr = consumer->OpenSharedFence(shared, IID_PPV_ARGS(&readFence));
            CloseHandle(shared);
        }
        if (FAILED(hr)) return hr;

        D3D11_TEXTURE2D_DESC desc = {};
        desc.Width = width;
        desc.Height = height;
        desc.MipLevels = 1;
        desc.ArraySize = 1;
        desc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
        desc.SampleDesc.Count = 1;
        desc.BindFlags = D3D11_BIND_RENDER_TARGET;
        desc.MiscFlags = D3D11_RESOURCE_MISC_SHARED;
        ComPtr<ID3D11Texture2D> texture;
        ComPtr<IDXGIResource> resource;
        hr = device->CreateTexture2D(&desc, nullptr, &texture);
        if (SUCCEEDED(hr)) hr = device->CreateRenderTargetView(texture.Get(), nullptr, &producerView);
        if (SUCCEEDED(hr)) hr = texture.As(&resource);
        if (SUCCEEDED(hr)) hr = resource->GetSharedHandle(&shared);
        if (SUCCEEDED(hr)) hr = consumer->OpenSharedResource(shared, IID_PPV_ARGS(&readTexture));
        if (FAILED(hr)) return hr;

        work = CreateEventW(nullptr, FALSE, FALSE, nullptr);
        timer = CreateWaitableTimerExW(nullptr, nullptr, CREATE_WAITABLE_TIMER_HIGH_RESOLUTION, TIMER_ALL_ACCESS);
        if (!work || !timer) return HRESULT_FROM_WIN32(GetLastError());
        thread = std::thread([this] {
            while (WaitForSingleObject(work, INFINITE) == WAIT_OBJECT_0 && !stopping) {
                const auto value = frame.load();
                if (!waitUntil(timer, release.load(), 350)) return;
                const float color[] = {0.08f, value % 2 ? 0.12f : 0.16f, 0.15f, 1.0f};
                producerContext->ClearRenderTargetView(producerView.Get(), color);
                if (FAILED(producerContext->Signal(producerFence.Get(), value))) return;
                producerContext->Flush();
            }
        });
        return S_OK;
    }

    void submit(uint64_t value, uint64_t releaseUs)
    {
        release = releaseUs;
        frame = value;
        SetEvent(work);
    }

    ComPtr<ID3D11Fence> readFence;
    ComPtr<ID3D11Texture2D> readTexture;

private:
    ComPtr<ID3D11DeviceContext4> producerContext;
    ComPtr<ID3D11Fence> producerFence;
    ComPtr<ID3D11RenderTargetView> producerView;
    std::atomic<uint64_t> frame{0}, release{0};
    std::atomic<bool> stopping{false};
    HANDLE work = nullptr, timer = nullptr;
    std::thread thread;
};
}

// This is a manual fullscreen hardware test, deliberately not a tst_* target.
// --help is noninteractive; --run is required to open a window.
int wmain(int argc, wchar_t** argv)
{
    bool run = false, check = false, scheduled = false;
    const wchar_t* decodeSetup = nullptr;
    unsigned fps = 116, seconds = 10;
    unsigned leadUs = 0;
    const wchar_t* output = L"composition-probe.json";
    for (int i = 1; i < argc; ++i) {
        if (!wcscmp(argv[i], L"--run")) run = true;
        else if (!wcscmp(argv[i], L"--check")) check = true;
        else if (!wcscmp(argv[i], L"--fps") && i + 1 < argc) fps = wcstoul(argv[++i], nullptr, 10);
        else if (!wcscmp(argv[i], L"--seconds") && i + 1 < argc) seconds = wcstoul(argv[++i], nullptr, 10);
        else if (!wcscmp(argv[i], L"--decode-setup") && i + 1 < argc) {
            decodeSetup = argv[++i];
            if (wcscmp(decodeSetup, L"serial") && wcscmp(decodeSetup, L"overlap") &&
                    wcscmp(decodeSetup, L"queued")) return 1;
        }
        else if (!wcscmp(argv[i], L"--lead-us") && i + 1 < argc) {
            const wchar_t* value = argv[++i];
            wchar_t* end = nullptr;
            leadUs = wcstoul(value, &end, 10);
            if (value == end || *end || *value < L'0' || *value > L'9') {
                std::fprintf(stderr, "--lead-us requires a nonnegative integer\n");
                return 1;
            }
            scheduled = true;
        }
        else if (!wcscmp(argv[i], L"--output") && i + 1 < argc) output = argv[++i];
        else if (wcscmp(argv[i], L"--help")) { std::fprintf(stderr, "Unknown or incomplete option\n"); return 1; }
    }
    if (!run && !check) {
        std::puts("compositionprobe --run [--fps 116] [--seconds 10] [--lead-us N] [--output result.json]\n"
                  "Manual Windows 11 fullscreen presentation test. Escape ends the test.\n"
                  "Use --lead-us to submit a scheduled target N microseconds early (less than a source period).\n"
                  "Omit --lead-us for immediate presentation. Reports OS timing and presentation mode counts.");
        std::puts("compositionprobe --check: verify runtime, driver, and buffer setup without showing a window.");
        std::puts("Use --decode-setup serial, overlap, or queued with --run for a fixed-target synthetic decode test.");
        return 0;
    }
    if (fps < 30 || fps > 360 || seconds < 3 || seconds > 60) return 1;
    if (decodeSetup && scheduled) return 1;
    if (scheduled && leadUs >= 1000000 / fps) {
        std::fprintf(stderr, "--lead-us must be less than one source period\n");
        return 1;
    }
    const bool overlap = decodeSetup && wcscmp(decodeSetup, L"serial");
    const bool queueWhileRendering = decodeSetup && !wcscmp(decodeSetup, L"queued");
    scheduled = scheduled || decodeSetup;
    const HINSTANCE instance = GetModuleHandleW(nullptr);
    WNDCLASSW wc = {};
    wc.hInstance = instance;
    wc.lpfnWndProc = windowProcedure;
    wc.lpszClassName = L"MoonlightCompositionProbe";
    if (!RegisterClassW(&wc)) return 1;
    MONITORINFO monitor = {};
    monitor.cbSize = sizeof(monitor);
    if (!GetMonitorInfoW(MonitorFromPoint({0, 0}, MONITOR_DEFAULTTOPRIMARY), &monitor)) return 1;
    const UINT width = monitor.rcMonitor.right - monitor.rcMonitor.left;
    const UINT height = monitor.rcMonitor.bottom - monitor.rcMonitor.top;
    HWND window = CreateWindowExW(0, wc.lpszClassName, L"Moonlight presentation test (Escape to close)",
        WS_POPUP, monitor.rcMonitor.left, monitor.rcMonitor.top, width, height,
        nullptr, nullptr, instance, nullptr);
    if (!window) return 1;
    if (run) ShowWindow(window, SW_SHOW);

    ComPtr<ID3D11Device> device;
    ComPtr<ID3D11DeviceContext> context;
    HRESULT hr = D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr,
        D3D11_CREATE_DEVICE_BGRA_SUPPORT | D3D11_CREATE_DEVICE_PREVENT_INTERNAL_THREADING_OPTIMIZATIONS,
        nullptr, 0, D3D11_SDK_VERSION, &device, nullptr, &context);
    ComPtr<ID3D11Device5> device5;
    ComPtr<ID3D11DeviceContext4> context4;
    ComPtr<ID3D11Fence> fence;
    if (SUCCEEDED(hr)) hr = device.As(&device5);
    if (SUCCEEDED(hr)) hr = context.As(&context4);
    if (SUCCEEDED(hr)) hr = device5->CreateFence(0, D3D11_FENCE_FLAG_NONE, IID_PPV_ARGS(&fence));
    if (FAILED(hr)) { std::fprintf(stderr, "D3D11 setup failed: %08lx\n", static_cast<unsigned long>(hr)); return 1; }

    if (check && !run) {
        std::printf("Runtime supported: %d; device supports independent flip: %d\n",
                    D3D11CompositionPresenter::runtimeSupported(),
                    D3D11CompositionPresenter::deviceSupported(device.Get()));
        bool passed = true;
        for (const auto format : {DXGI_FORMAT_R8G8B8A8_UNORM, DXGI_FORMAT_R10G10B10A2_UNORM}) {
            D3D11CompositionPresenter presenter;
            hr = presenter.initialize(device.Get(), window, width, height, format, {}, 0);
            std::printf("Presentation setup, format %u: %08lx\n", unsigned(format),
                        static_cast<unsigned long>(hr));
            passed = passed && SUCCEEDED(hr);
        }
        DestroyWindow(window);
        return passed ? 0 : 1;
    }

    // Resolve the active path for the primary monitor, not an assumed source 0.
    UINT pathCount = 0, modeCount = 0;
    if (GetDisplayConfigBufferSizes(QDC_ONLY_ACTIVE_PATHS, &pathCount, &modeCount) != ERROR_SUCCESS) return 1;
    std::vector<DISPLAYCONFIG_PATH_INFO> paths(pathCount);
    std::vector<DISPLAYCONFIG_MODE_INFO> modes(modeCount);
    if (QueryDisplayConfig(QDC_ONLY_ACTIVE_PATHS, &pathCount, paths.data(), &modeCount, modes.data(), nullptr) != ERROR_SUCCESS) return 1;
    const DISPLAYCONFIG_PATH_INFO* selected = nullptr;
    for (UINT i = 0; i < pathCount; ++i) {
        const auto index = paths[i].sourceInfo.modeInfoIdx;
        if (index < modeCount && modes[index].infoType == DISPLAYCONFIG_MODE_INFO_TYPE_SOURCE &&
                modes[index].sourceMode.position.x == monitor.rcMonitor.left &&
                modes[index].sourceMode.position.y == monitor.rcMonitor.top) {
            if (selected) { std::fprintf(stderr, "Cloned/ambiguous outputs are not supported by this probe\n"); return 1; }
            selected = &paths[i];
        }
    }
    if (!selected || !selected->targetInfo.refreshRate.Numerator) return 1;
    const uint64_t refreshUs = uint64_t(selected->targetInfo.refreshRate.Denominator) * 1000000 /
        selected->targetInfo.refreshRate.Numerator;
    if (!refreshUs || 1000000 / fps < refreshUs) return 1;
    D3D11CompositionPresenter presenter;
    hr = presenter.initialize(device.Get(), window, width, height, DXGI_FORMAT_R8G8B8A8_UNORM,
        selected->sourceInfo.adapterId, selected->sourceInfo.id);
    if (FAILED(hr)) { std::fprintf(stderr, "Composition setup failed: %08lx\n", static_cast<unsigned long>(hr)); return 1; }

    DecodeProducer decoder;
    if (decodeSetup) {
        hr = decoder.initialize(device5.Get(), width, height);
        if (FAILED(hr)) {
            std::fprintf(stderr, "Shared decode setup failed: %08lx\n", static_cast<unsigned long>(hr));
            return 1;
        }
    }

    HANDLE ready = CreateEventW(nullptr, FALSE, FALSE, nullptr);
    HANDLE decoded = decodeSetup ? CreateEventW(nullptr, FALSE, FALSE, nullptr) : nullptr;
    HANDLE timer = CreateWaitableTimerExW(nullptr, nullptr, CREATE_WAITABLE_TIMER_HIGH_RESOLUTION, TIMER_ALL_ACCESS);
    if (!ready || !timer || (decodeSetup && !decoded)) return 1;
    struct Submission { uint64_t id = 0, at = 0, target = 0, sourceFrame = 0; };
    std::array<Submission, 128> pending = {};
    std::vector<uint64_t> latencies, intervals, intervalJerk, absoluteTargetErrors;
    std::vector<int64_t> submissionLeads, targetErrors;
    uint64_t lastDisplayed = 0, lastDisplayedSourceFrame = 0, lastInterval = 0;
    uint64_t submitted = 0, measuredSubmissions = 0, unavailable = 0, lateSubmissions = 0;
    uint64_t leadAtLeast1ms = 0, leadAtLeast2ms = 0;
    uint64_t flushedBeforeDecodeReady = 0;
    uint64_t acceptedBeforeRenderReady = 0;
    const uint64_t start = clockUs();
    const auto collectDisplayFeedback = [&] {
        D3D11CompositionPresenter::DisplayedFrame frame;
        while (presenter.pollDisplayedFrame(clockUs, frame)) {
            const auto& p = pending[frame.id % pending.size()];
            if (p.id != frame.id || p.at < start + 1000000 || frame.clock.timeUs < p.at) continue;
            latencies.push_back(frame.clock.timeUs - p.at);
            if (scheduled) {
                const auto error = differenceUs(frame.clock.timeUs, p.target);
                targetErrors.push_back(error);
                absoluteTargetErrors.push_back(error < 0 ? -error : error);
            }
            uint64_t interval = 0;
            if (lastDisplayed && p.sourceFrame == lastDisplayedSourceFrame + 1 && frame.clock.timeUs > lastDisplayed) {
                interval = frame.clock.timeUs - lastDisplayed;
                intervals.push_back(interval);
                if (lastInterval) {
                    intervalJerk.push_back(interval > lastInterval ? interval - lastInterval : lastInterval - interval);
                }
            }
            lastInterval = interval;
            lastDisplayed = frame.clock.timeUs;
            lastDisplayedSourceFrame = p.sourceFrame;
        }
    };
    uint64_t number = 0;
    bool stopped = false;
    while (!stopped && clockUs() - start < uint64_t(seconds) * 1000000) {
        MSG message;
        while (PeekMessageW(&message, nullptr, 0, 0, PM_REMOVE)) {
            if (message.message == WM_QUIT) stopped = true;
            TranslateMessage(&message);
            DispatchMessageW(&message);
        }
        if (stopped) break;
        const uint64_t target = start + ++number * 1000000 / fps;
        const auto observeDecode = [&] {
            const auto completed = decoder.readFence->GetCompletedValue();
            if (completed == UINT64_MAX) return false;
            if (completed >= number) return true;
            return SUCCEEDED(decoder.readFence->SetEventOnCompletion(number, decoded)) &&
                   WaitForSingleObject(decoded, 50) == WAIT_OBJECT_0 &&
                   decoder.readFence->GetCompletedValue() == number;
        };
        if (decodeSetup) {
            if (!waitUntil(timer, target - 3200, 350)) { hr = E_FAIL; break; }
            decoder.submit(number, target - 1300);
            if (!overlap && !observeDecode()) { hr = E_FAIL; break; }
        }
        ComPtr<ID3D11RenderTargetView> view;
        ComPtr<ID3D11Resource> outputResource;
        hr = presenter.acquire(&view);
        if (hr == S_FALSE) ++unavailable;
        else if (FAILED(hr)) break;
        if (view && decodeSetup) view->GetResource(&outputResource);
        // A frame with no available output still completes its producer
        // request before we reuse the single shared texture on the next tick.
        if (decodeSetup && overlap && !view && !observeDecode()) { hr = E_FAIL; break; }
        const auto finishRendering = [&] {
            return WaitForSingleObject(ready, 50) == WAIT_OBJECT_0 &&
                   fence->GetCompletedValue() == number;
        };
        if (view) {
            const float background[4] = {0.08f, 0.08f, 0.08f, 1.0f};
            const float bar[4] = {0.7f, 0.7f, 0.7f, 1.0f};
            if (decodeSetup) {
                // In the overlap arm the CPU records and flushes this batch
                // while decode is pending. Only the GPU waits before the first
                // read. The render fence retires the read before the producer
                // can write this shared texture again.
                hr = context4->Wait(decoder.readFence.Get(), number);
                if (FAILED(hr)) break;
                context4->CopyResource(outputResource.Get(), decoder.readTexture.Get());
            }
            else {
                context4->ClearRenderTargetView(view.Get(), background);
            }
            const LONG x = static_cast<LONG>(number * 8 % width);
            const D3D11_RECT rect = {x, 0, std::min<LONG>(x + 80, width), static_cast<LONG>(height)};
            context4->ClearView(view.Get(), bar, &rect, 1);
            hr = context4->Signal(fence.Get(), number);
            context4->Flush();
            if (decodeSetup && target >= start + 1000000 &&
                    decoder.readFence->GetCompletedValue() < number) {
                ++flushedBeforeDecodeReady;
            }
            if (SUCCEEDED(hr)) hr = fence->SetEventOnCompletion(number, ready);
            if (FAILED(hr) || (!queueWhileRendering && !finishRendering())) { hr = E_FAIL; break; }
        }
        if (!decodeSetup && !waitUntil(timer, target - leadUs)) {
            hr = E_FAIL;
            break;
        }
        if (view) {
            const auto at = clockUs();
            uint64_t id;
            D3D11CompositionPresenter::PresentResult observation;
            hr = scheduled ? presenter.present(target, clockUs, id, &observation) : presenter.present(id);
            if (queueWhileRendering) {
                // The presentation manager orders display after the drawing
                // commands already issued to its device. The application
                // still owns the source read and must verify its retirement
                // before the single producer texture is used again.
                if (hr == S_OK && observation.submissionTimeUs >= start + 1000000 &&
                        fence->GetCompletedValue() < number) {
                    ++acceptedBeforeRenderReady;
                }
                if (!finishRendering()) {
                    hr = E_FAIL;
                    break;
                }
            }
            if (FAILED(hr)) break;
            if (hr == S_OK) {
                const auto submittedAt = scheduled ? observation.submissionTimeUs : at;
                pending[id % pending.size()] = {id, submittedAt, target, number};
                ++submitted;
                if (submittedAt >= start + 1000000) {
                    ++measuredSubmissions;
                    if (scheduled) {
                        const auto lead = differenceUs(target, submittedAt);
                        submissionLeads.push_back(lead);
                        if (lead <= 0) ++lateSubmissions;
                        if (lead >= 1000) ++leadAtLeast1ms;
                        if (lead >= 2000) ++leadAtLeast2ms;
                    }
                }
            }
        }
        collectDisplayFeedback();
    }
    if (SUCCEEDED(hr) && !stopped) {
        // Give the last queued target and delayed flip statistics time to arrive.
        const auto end = clockUs() + 100000;
        do {
            collectDisplayFeedback();
            Sleep(1);
        } while (clockUs() < end);
    }
    const bool coverage = measuredSubmissions && latencies.size() * 100 >= measuredSubmissions * 90;
    const bool belowOneRefresh = coverage && percentile(latencies, 99) < refreshUs;
    FILE* report = nullptr;
    _wfopen_s(&report, output, L"w");
    if (report) {
        std::fprintf(report,
            "{\n  \"submitted\": %llu, \"steady_submitted\": %llu, \"measured\": %llu,\n"
            "  \"independent_flip_frames\": %llu, \"composed_frames\": %llu, \"unavailable_buffers\": %llu,\n"
            "  \"rejected_display_timestamps\": %llu,\n"
            "  \"skipped_presents\": %llu, \"canceled_presents\": %llu,\n"
            "  \"display_period_us\": %llu, \"submit_to_display_p50_us\": %llu, \"submit_to_display_p95_us\": %llu, \"submit_to_display_p99_us\": %llu,\n"
            "  \"display_interval_p50_us\": %llu, \"display_interval_p99_us\": %llu,\n"
            "  \"display_interval_jerk_p99_us\": %llu,\n"
            "  \"coverage_at_least_90_percent\": %s, \"p99_service_below_one_refresh\": %s,\n"
            "  \"optical_vrr_verified\": false, \"run_completed\": %s",
            static_cast<unsigned long long>(submitted), static_cast<unsigned long long>(measuredSubmissions),
            static_cast<unsigned long long>(latencies.size()), static_cast<unsigned long long>(presenter.independentFrames()),
            static_cast<unsigned long long>(presenter.composedFrames()), static_cast<unsigned long long>(unavailable),
            static_cast<unsigned long long>(presenter.rejectedDisplayFrames()),
            static_cast<unsigned long long>(presenter.skippedFrames()),
            static_cast<unsigned long long>(presenter.canceledFrames()),
            static_cast<unsigned long long>(refreshUs), static_cast<unsigned long long>(percentile(latencies, 50)),
            static_cast<unsigned long long>(percentile(latencies, 95)), static_cast<unsigned long long>(percentile(latencies, 99)),
            static_cast<unsigned long long>(percentile(intervals, 50)), static_cast<unsigned long long>(percentile(intervals, 99)),
            static_cast<unsigned long long>(percentile(intervalJerk, 99)),
            coverage ? "true" : "false", belowOneRefresh ? "true" : "false",
            SUCCEEDED(hr) && !stopped ? "true" : "false");
        if (scheduled) {
            std::fprintf(report,
                ",\n  \"requested_lead_us\": %u, \"submitted_at_or_after_target\": %llu,\n"
                "  \"submission_lead_p05_us\": %lld, \"submission_lead_p50_us\": %lld,\n"
                "  \"display_minus_target_p50_us\": %lld, \"absolute_target_error_p99_us\": %llu",
                leadUs, static_cast<unsigned long long>(lateSubmissions),
                static_cast<long long>(percentile(submissionLeads, 5)),
                static_cast<long long>(percentile(submissionLeads, 50)),
                static_cast<long long>(percentile(targetErrors, 50)),
                static_cast<unsigned long long>(percentile(absoluteTargetErrors, 99)));
        }
        if (decodeSetup) {
            std::fprintf(report,
                ",\n  \"synthetic_decode_setup\": \"%ls\", \"source_fps\": %u,"
                "\n  \"render_batches_flushed_before_decode_ready\": %llu,"
                "\n  \"presents_accepted_before_render_ready\": %llu,"
                "\n  \"lead_at_least_1ms\": %llu, \"lead_at_least_2ms\": %llu",
                decodeSetup, fps, static_cast<unsigned long long>(flushedBeforeDecodeReady),
                static_cast<unsigned long long>(acceptedBeforeRenderReady),
                static_cast<unsigned long long>(leadAtLeast1ms),
                static_cast<unsigned long long>(leadAtLeast2ms));
        }
        std::fputs("\n}\n", report);
        std::fclose(report);
    }
    presenter.reset();
    CloseHandle(ready);
    if (decoded) CloseHandle(decoded);
    CloseHandle(timer);
    DestroyWindow(window);
    return report && SUCCEEDED(hr) && !stopped && (scheduled ? coverage : belowOneRefresh) ? 0 : 2;
}
