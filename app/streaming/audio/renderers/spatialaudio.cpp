// Windows Spatial Audio integration for Moonlight's decoded 7.1 PCM.
// API contract: https://learn.microsoft.com/windows/win32/coreaudio/render-spatial-sound-using-spatial-audio-objects
// All rendering COM interfaces are created, used, and released on the worker.
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <initguid.h>
#include <mmdeviceapi.h>
#include <functiondiscoverykeys_devpkey.h>
#include <spatialaudioclient.h>
#include <avrt.h>
#include <wrl/client.h>

#include "spatialaudio.h"
#include "spatialaudioqueue.h"
#include "SDL_compat.h"

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstring>
#include <cwchar>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

using Microsoft::WRL::ComPtr;

namespace {
static_assert(Spatial71::StaticObjectTypes[0] == AudioObjectType_FrontLeft &&
              Spatial71::StaticObjectTypes[1] == AudioObjectType_FrontRight &&
              Spatial71::StaticObjectTypes[2] == AudioObjectType_FrontCenter &&
              Spatial71::StaticObjectTypes[3] == AudioObjectType_LowFrequency &&
              Spatial71::StaticObjectTypes[4] == AudioObjectType_BackLeft &&
              Spatial71::StaticObjectTypes[5] == AudioObjectType_BackRight &&
              Spatial71::StaticObjectTypes[6] == AudioObjectType_SideLeft &&
              Spatial71::StaticObjectTypes[7] == AudioObjectType_SideRight,
              "7.1 PCM channel order must match the native static object types");

struct SpatialFailure {
    const char* operation;
    HRESULT result;
};

void require(HRESULT result, const char* operation)
{
    if (FAILED(result)) {
        throw SpatialFailure{operation, result};
    }
}

LONGLONG qpc()
{
    LARGE_INTEGER value{};
    QueryPerformanceCounter(&value);
    return value.QuadPart;
}

struct Apartment {
    bool initialized = false;
    explicit Apartment(bool allowExistingApartment = false)
    {
        const HRESULT hr = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
        // Read-only enumeration may be called from Qt's already initialized
        // STA. Rendering always uses its own MTA worker and disallows this path.
        if (allowExistingApartment && hr == RPC_E_CHANGED_MODE) return;
        require(hr, "CoInitializeEx");
        initialized = true;
    }
    ~Apartment() { if (initialized) CoUninitialize(); }
};

struct Event {
    HANDLE handle;
    explicit Event(bool manualReset) : handle(CreateEventW(nullptr, manualReset, FALSE, nullptr))
    {
        if (!handle) {
            throw SpatialFailure{"CreateEventW", HRESULT_FROM_WIN32(GetLastError())};
        }
    }
    ~Event() { CloseHandle(handle); }
    Event(const Event&) = delete;
    Event& operator=(const Event&) = delete;
};

struct Mmcss {
    DWORD index = 0;
    HANDLE task = AvSetMmThreadCharacteristicsW(L"Pro Audio", &index);
    DWORD error = task ? ERROR_SUCCESS : GetLastError();
    ~Mmcss() { if (task) AvRevertMmThreadCharacteristics(task); }
};

struct Variant {
    PROPVARIANT value{};
    ~Variant() { PropVariantClear(&value); }
};

struct DeviceId {
    LPWSTR value = nullptr;
    explicit DeviceId(IMMDevice* device)
    {
        const HRESULT hr = device->GetId(&value);
        if (FAILED(hr) || !value || !*value) {
            CoTaskMemFree(value);
            value = nullptr;
            throw SpatialFailure{"Read endpoint ID", FAILED(hr) ? hr : E_UNEXPECTED};
        }
    }
    ~DeviceId() { CoTaskMemFree(value); }
    DeviceId(const DeviceId&) = delete;
    DeviceId& operator=(const DeviceId&) = delete;
};

bool isDigitalDisplay(IPropertyStore* properties)
{
    Variant formFactor;
    require(properties->GetValue(PKEY_AudioEndpoint_FormFactor, &formFactor.value), "Read endpoint form factor");
    return formFactor.value.vt == VT_UI4 && formFactor.value.ulVal == DigitalAudioDisplayDevice;
}

struct SpatialStream {
    ComPtr<ISpatialAudioObjectRenderStream> stream;
    std::array<ComPtr<ISpatialAudioObject>, Spatial71::ChannelCount> objects;
    bool started = false;
    ~SpatialStream()
    {
        if (!stream) return;
        if (started) {
            const HRESULT hr = stream->Stop();
            if (FAILED(hr)) {
                SDL_LogWarn(SDL_LOG_CATEGORY_APPLICATION,
                            "Spatial 7.1: Stop during cleanup failed: 0x%08lX", static_cast<unsigned long>(hr));
            }
        }
        for (auto& object : objects) object.Reset();
        const HRESULT hr = stream->Reset();
        if (FAILED(hr)) {
            SDL_LogWarn(SDL_LOG_CATEGORY_APPLICATION,
                        "Spatial 7.1: Reset during cleanup failed: 0x%08lX", static_cast<unsigned long>(hr));
        }
    }
};

// Even an exception in GetBuffer must end the update before any object is
// released. This guard is destroyed before SpatialStream on every path.
struct UpdatePass {
    ISpatialAudioObjectRenderStream* stream;
    bool updating = false;
    UpdatePass(ISpatialAudioObjectRenderStream* value, UINT32& frames) : stream(value)
    {
        UINT32 dynamicObjects = 0;
        require(stream->BeginUpdatingAudioObjects(&dynamicObjects, &frames), "BeginUpdatingAudioObjects");
        updating = true;
    }
    void finish()
    {
        const HRESULT hr = stream->EndUpdatingAudioObjects();
        updating = false;
        require(hr, "EndUpdatingAudioObjects");
    }
    ~UpdatePass() { if (updating) stream->EndUpdatingAudioObjects(); }
};
} // namespace

struct SpatialAudioRenderer::Impl
{
    explicit Impl(const QString& id) : endpointId(id.toStdWString()) {}

    ~Impl()
    {
        stop();
        // Worker has joined: no audio-loop logging, and no counter races.
        if (workerCreated) {
            const auto stats = queue.statistics();
            const double tickMs = frequency ? 1000.0 / static_cast<double>(frequency) : 0;
            SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION,
                "Spatial 7.1 stats: submitted=%llu consumed=%llu dropped=%llu underrun_frames=%llu "
                "queue_peak=%zu/1440 frames quanta=%llu quantum_max=%u startup_silence_frames=%llu "
                "max_event_interval_ms=%.3f max_fill_ms=%.3f failed=%d (not acoustic latency)",
                static_cast<unsigned long long>(stats.submittedFrames),
                static_cast<unsigned long long>(stats.consumedFrames),
                static_cast<unsigned long long>(stats.droppedFrames),
                static_cast<unsigned long long>(underrunFrames), stats.highWaterFrames,
                static_cast<unsigned long long>(quanta), maxQuantum,
                static_cast<unsigned long long>(startupSilenceFrames),
                static_cast<double>(maxEventInterval) * tickMs,
                static_cast<double>(maxFill) * tickMs, failed.load() ? 1 : 0);
        }
    }

    void stop()
    {
        if (stopEvent) SetEvent(stopEvent->handle);
        // The event wait is cancellable. Windows COM/driver calls themselves
        // are not: never detach a thread that could still reference this state.
        if (worker.joinable()) worker.join();
    }

    void fail(const char* operation, HRESULT hr)
    {
        if (!failed.exchange(true)) {
            SDL_LogError(SDL_LOG_CATEGORY_APPLICATION,
                         "Spatial 7.1: %s failed: 0x%08lX; renderer recovery required",
                         operation, static_cast<unsigned long>(hr));
        }
        if (stopEvent) SetEvent(stopEvent->handle);
    }

    void completeInitialization(bool success)
    {
        std::lock_guard<std::mutex> lock(initMutex);
        if (!initCompleted) {
            initSuccess = success;
            initCompleted = true;
            initCondition.notify_one();
        }
    }

    void run() noexcept
    {
        try {
            runStream();
        }
        catch (const SpatialFailure& error) {
            fail(error.operation, error.result);
        }
        catch (const std::bad_alloc&) {
            fail("Worker allocation", E_OUTOFMEMORY);
        }
        catch (...) {
            fail("Unexpected worker exception", E_UNEXPECTED);
        }
        completeInitialization(false);
    }

    void runStream()
    {
        Apartment apartment;
        ComPtr<IMMDeviceEnumerator> enumerator;
        require(CoCreateInstance(__uuidof(MMDeviceEnumerator), nullptr, CLSCTX_INPROC_SERVER,
                                 IID_PPV_ARGS(&enumerator)), "Create MMDevice enumerator");
        ComPtr<IMMDevice> device;
        if (endpointId.empty()) {
            require(enumerator->GetDefaultAudioEndpoint(eRender, eConsole, &device), "Get default console render endpoint");
        }
        else {
            require(enumerator->GetDevice(endpointId.c_str(), &device), "Get explicit endpoint");
        }
        const DeviceId actualId(device.Get());
        if (!endpointId.empty() && endpointId != actualId.value) {
            throw SpatialFailure{"Endpoint ID mismatch", E_INVALIDARG};
        }

        DWORD state = 0;
        require(device->GetState(&state), "Read endpoint state");
        ComPtr<IMMEndpoint> endpoint;
        require(device.As(&endpoint), "Query IMMEndpoint");
        EDataFlow flow = eAll;
        require(endpoint->GetDataFlow(&flow), "Read endpoint data flow");
        if (state != DEVICE_STATE_ACTIVE || flow != eRender) {
            throw SpatialFailure{"Endpoint is not an active render output", E_INVALIDARG};
        }
        ComPtr<IPropertyStore> properties;
        require(device->OpenPropertyStore(STGM_READ, &properties), "Read endpoint properties");
        if (!isDigitalDisplay(properties.Get())) {
            throw SpatialFailure{"Selected endpoint is not an HDMI/DisplayPort output", E_INVALIDARG};
        }

        ComPtr<ISpatialAudioClient> client;
        require(device->Activate(__uuidof(ISpatialAudioClient), CLSCTX_INPROC_SERVER, nullptr,
                                  reinterpret_cast<void**>(client.GetAddressOf())), "Activate ISpatialAudioClient");
        AudioObjectType nativeMask = AudioObjectType_None;
        require(client->GetNativeStaticObjectTypeMask(&nativeMask), "Read native static object mask");
        if ((static_cast<UINT32>(nativeMask) & Spatial71::StaticObjectMask) != Spatial71::StaticObjectMask) {
            throw SpatialFailure{"Endpoint lacks eight native 7.1 objects", AUDCLNT_E_UNSUPPORTED_FORMAT};
        }

        WAVEFORMATEX format{};
        format.wFormatTag = WAVE_FORMAT_IEEE_FLOAT;
        format.nChannels = 1;
        format.nSamplesPerSec = static_cast<DWORD>(Spatial71::SampleRate);
        format.wBitsPerSample = 32;
        format.nBlockAlign = sizeof(float);
        format.nAvgBytesPerSec = format.nSamplesPerSec * format.nBlockAlign;
        const HRESULT supported = client->IsAudioObjectFormatSupported(&format);
        if (supported != S_OK) {
            throw SpatialFailure{"Exact mono float32/48 kHz object format unavailable",
                                 FAILED(supported) ? supported : AUDCLNT_E_UNSUPPORTED_FORMAT};
        }
        UINT32 maximumFrames = 0;
        require(client->GetMaxFrameCount(&format, &maximumFrames), "GetMaxFrameCount");
        if (maximumFrames == 0 || maximumFrames > Spatial71::SampleRate) {
            throw SpatialFailure{"Invalid maximum spatial quantum", E_UNEXPECTED};
        }
        // All allocations are complete before playback. COM buffers are never
        // retained beyond their update; this scratch buffer owns interleaved PCM.
        std::vector<float> scratch(static_cast<std::size_t>(maximumFrames) * Spatial71::ChannelCount);
        Event renderEvent(false);
        SpatialStream output;
        SpatialAudioObjectRenderStreamActivationParams parameters{};
        parameters.ObjectFormat = &format;
        parameters.StaticObjectTypeMask = static_cast<AudioObjectType>(Spatial71::StaticObjectMask);
        parameters.MinDynamicObjectCount = 0;
        parameters.MaxDynamicObjectCount = 0;
        parameters.Category = AudioCategory_GameMedia;
        parameters.EventHandle = renderEvent.handle;
        PROPVARIANT activation{};
        activation.vt = VT_BLOB;
        activation.blob.cbSize = sizeof(parameters);
        activation.blob.pBlobData = reinterpret_cast<BYTE*>(&parameters);
        // activation refers to stack storage; it must not be PropVariantClear'd.
        require(client->ActivateSpatialAudioStream(&activation, __uuidof(ISpatialAudioObjectRenderStream),
            reinterpret_cast<void**>(output.stream.GetAddressOf())), "Activate 7.1 spatial stream");
        LARGE_INTEGER counterFrequency{};
        if (!QueryPerformanceFrequency(&counterFrequency) || counterFrequency.QuadPart <= 0) {
            throw SpatialFailure{"QueryPerformanceFrequency", E_UNEXPECTED};
        }
        frequency = counterFrequency.QuadPart;
        Mmcss mmcss;
        if (mmcss.error != ERROR_SUCCESS) {
            SDL_LogWarn(SDL_LOG_CATEGORY_APPLICATION,
                        "Spatial 7.1: MMCSS Pro Audio unavailable: %lu", mmcss.error);
        }
        require(output.stream->Start(), "Start spatial stream");
        output.started = true;
        const HANDLE events[] = {stopEvent->handle, renderEvent.handle};
        LONGLONG lastEvent = 0;
        bool activated = false;
        unsigned zeroQuanta = 0;

        for (;;) {
            const DWORD wait = WaitForMultipleObjects(2, events, FALSE, 1000);
            if (wait == WAIT_OBJECT_0) break;
            if (wait != WAIT_OBJECT_0 + 1) {
                throw SpatialFailure{"Spatial event wait", wait == WAIT_FAILED ?
                                     HRESULT_FROM_WIN32(GetLastError()) : HRESULT_FROM_WIN32(WAIT_TIMEOUT)};
            }
            const LONGLONG ready = qpc();
            if (lastEvent) maxEventInterval = (std::max)(maxEventInterval, ready - lastEvent);
            lastEvent = ready;
            UINT32 frames = 0;
            UpdatePass pass(output.stream.Get(), frames);
            if (frames > maximumFrames || frames > Spatial71::QueueCapacityFrames) {
                throw SpatialFailure{"Spatial quantum exceeds buffer or 30 ms queue budget", AUDCLNT_E_UNSUPPORTED_FORMAT};
            }
            if (!frames && ++zeroQuanta > 100) {
                throw SpatialFailure{"Repeated empty spatial quanta", E_UNEXPECTED};
            }
            if (frames) zeroQuanta = 0;

            {
                // Only memory copies and counters run under this lock. No COM
                // call or wait can block the Opus callback while holding it.
                std::lock_guard<std::mutex> lock(queueMutex);
                if (!primed && queue.size() &&
                    (queue.size() >= Spatial71::StartupFrames ||
                     (firstQueued && ready - firstQueued >= frequency / 100))) {
                    primed = true;
                }
                if (primed) {
                    const auto copied = queue.pop(scratch.data(), frames);
                    everPlayed = everPlayed || copied != 0;
                    underrunFrames += frames - copied;
                    if (copied < frames || queue.size() == 0) {
                        primed = false;
                        firstQueued = 0;
                    }
                }
                else {
                    std::fill_n(scratch.data(), static_cast<std::size_t>(frames) * Spatial71::ChannelCount, 0.0f);
                    if (everPlayed) underrunFrames += frames;
                    else startupSilenceFrames += frames;
                }
            }

            // All eight objects are updated, including silent or muted periods.
            // Omitting GetBuffer can terminate a static object implicitly.
            for (std::size_t channel = 0; channel < Spatial71::ChannelCount; ++channel) {
                if (!output.objects[channel]) {
                    require(output.stream->ActivateSpatialAudioObject(
                        static_cast<AudioObjectType>(Spatial71::StaticObjectTypes[channel]),
                        &output.objects[channel]), "Activate static 7.1 object");
                }
                BYTE* bytes = nullptr;
                UINT32 length = 0;
                require(output.objects[channel]->GetBuffer(&bytes, &length), "Get static object buffer");
                if (length < frames * sizeof(float) || length > maximumFrames * sizeof(float) ||
                    length % sizeof(float) != 0 || (length && !bytes)) {
                    throw SpatialFailure{"Unexpected static object buffer extent", E_UNEXPECTED};
                }
                if (length) std::memset(bytes, 0, length);
                if (!Spatial71::deinterleave(scratch.data(), frames, channel, reinterpret_cast<float*>(bytes))) {
                    throw SpatialFailure{"Invalid channel buffer", E_UNEXPECTED};
                }
            }
            pass.finish();
            maxFill = (std::max)(maxFill, qpc() - ready);
            maxQuantum = (std::max)(maxQuantum, frames);
            ++quanta;
            if (!activated && frames) {
                activated = true;
                SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION,
                    "Spatial 7.1 active: 48000 Hz float32, FL FR FC LFE BL BR SL SR, "
                    "8 static objects, native_mask=0x%08X, maximum_frames=%u, queue_limit=1440 (30 ms), "
                    "startup_target=480 frames/10 ms (event-quantized), selection=%s; provider/physical Atmos not verified",
                    static_cast<unsigned int>(nativeMask), maximumFrames,
                    endpointId.empty() ? "default console render at initialization" : "explicit endpoint ID");
                completeInitialization(true);
            }
        }
    }

    const std::wstring endpointId;
    std::array<float, Spatial71::MaxDecodeFrames * Spatial71::ChannelCount> staging{};
    std::size_t decodeCapacityFrames = 0;
    std::unique_ptr<Event> stopEvent;
    std::thread worker;
    bool workerCreated = false;
    std::atomic<bool> failed{false};
    std::mutex initMutex;
    std::condition_variable initCondition;
    bool initCompleted = false;
    bool initSuccess = false;
    std::mutex queueMutex;
    Spatial71::FrameQueue queue;
    bool primed = false;
    bool everPlayed = false;
    LONGLONG firstQueued = 0;
    LONGLONG frequency = 0;
    LONGLONG maxEventInterval = 0;
    LONGLONG maxFill = 0;
    std::uint64_t quanta = 0;
    std::uint64_t startupSilenceFrames = 0;
    std::uint64_t underrunFrames = 0;
    UINT32 maxQuantum = 0;
};

QList<SpatialAudioRenderer::EndpointInfo> SpatialAudioRenderer::enumerateEndpoints(QString* error)
{
    if (error) error->clear();
    try {
        Apartment apartment(true);
        ComPtr<IMMDeviceEnumerator> enumerator;
        require(CoCreateInstance(__uuidof(MMDeviceEnumerator), nullptr, CLSCTX_INPROC_SERVER,
                                 IID_PPV_ARGS(&enumerator)), "Create MMDevice enumerator");
        QString defaultId;
        ComPtr<IMMDevice> defaultDevice;
        const HRESULT defaultResult = enumerator->GetDefaultAudioEndpoint(eRender, eConsole, &defaultDevice);
        if (SUCCEEDED(defaultResult)) {
            const DeviceId id(defaultDevice.Get());
            defaultId = QString::fromWCharArray(id.value);
        }
        else if (defaultResult != HRESULT_FROM_WIN32(ERROR_NOT_FOUND)) {
            require(defaultResult, "Read default console render endpoint");
        }

        ComPtr<IMMDeviceCollection> devices;
        require(enumerator->EnumAudioEndpoints(eRender, DEVICE_STATE_ACTIVE, &devices), "Enumerate active render endpoints");
        UINT count = 0;
        require(devices->GetCount(&count), "Count render endpoints");
        QList<EndpointInfo> result;
        for (UINT index = 0; index < count; ++index) {
            ComPtr<IMMDevice> device;
            require(devices->Item(index, &device), "Read enumerated endpoint");
            ComPtr<IPropertyStore> properties;
            require(device->OpenPropertyStore(STGM_READ, &properties), "Read endpoint properties");
            if (!isDigitalDisplay(properties.Get())) continue;
            const DeviceId id(device.Get());
            EndpointInfo info;
            info.id = QString::fromWCharArray(id.value);
            info.isDefault = info.id == defaultId;
            Variant friendlyName;
            require(properties->GetValue(PKEY_Device_FriendlyName, &friendlyName.value), "Read endpoint name");
            if (friendlyName.value.vt == VT_LPWSTR && friendlyName.value.pwszVal) {
                info.name = QString::fromWCharArray(friendlyName.value.pwszVal);
            }
            result.append(info);
        }
        return result;
    }
    catch (const SpatialFailure& failure) {
        SDL_LogError(SDL_LOG_CATEGORY_APPLICATION, "Spatial 7.1 endpoint enumeration: %s failed: 0x%08lX",
                     failure.operation, static_cast<unsigned long>(failure.result));
        if (error) *error = QStringLiteral("Endpoint enumeration failed; see the diagnostic log for the HRESULT.");
    }
    catch (const std::bad_alloc&) {
        if (error) *error = QStringLiteral("Not enough memory to enumerate audio endpoints.");
    }
    catch (...) {
        if (error) *error = QStringLiteral("Unexpected failure while enumerating audio endpoints.");
    }
    return {};
}

SpatialAudioRenderer::SpatialAudioRenderer(const QString& endpointId)
    : m_Impl(new Impl(endpointId))
{
}

SpatialAudioRenderer::~SpatialAudioRenderer() = default;

bool SpatialAudioRenderer::prepareForPlayback(const OPUS_MULTISTREAM_CONFIGURATION* config)
{
    if (m_Impl->workerCreated || !config || config->channelCount != static_cast<int>(Spatial71::ChannelCount) ||
        config->sampleRate != static_cast<int>(Spatial71::SampleRate) || config->samplesPerFrame <= 0 ||
        static_cast<std::size_t>(config->samplesPerFrame) > Spatial71::MaxDecodeFrames ||
        m_Impl->endpointId.size() > 4096 ||
        m_Impl->endpointId.find(L'\0') != std::wstring::npos) {
        SDL_LogWarn(SDL_LOG_CATEGORY_APPLICATION,
                    "Spatial 7.1 requires eight channels at 48000 Hz and a valid optional endpoint ID");
        return false;
    }
    m_Impl->decodeCapacityFrames = static_cast<std::size_t>(config->samplesPerFrame);
    try {
        m_Impl->stopEvent.reset(new Event(true));
        m_Impl->worker = std::thread([this] { m_Impl->run(); });
        m_Impl->workerCreated = true;
    }
    catch (const SpatialFailure& error) {
        m_Impl->fail(error.operation, error.result);
        return false;
    }
    catch (...) {
        m_Impl->fail("Create spatial worker", E_OUTOFMEMORY);
        return false;
    }

    bool completed = false;
    bool success = false;
    try {
        std::unique_lock<std::mutex> lock(m_Impl->initMutex);
        completed = m_Impl->initCondition.wait_for(lock, std::chrono::seconds(5),
                                                  [this] { return m_Impl->initCompleted; });
        success = completed && m_Impl->initSuccess && !m_Impl->failed.load();
    }
    catch (...) {
        m_Impl->fail("Wait for spatial worker initialization", E_FAIL);
        m_Impl->stop();
        return false;
    }
    if (!success) {
        if (!completed) m_Impl->fail("Spatial worker initialization timeout", HRESULT_FROM_WIN32(WAIT_TIMEOUT));
        m_Impl->stop();
    }
    return success;
}

void* SpatialAudioRenderer::getAudioBuffer(int* size)
{
    std::size_t frames = 0;
    if (!size || !Spatial71::bytesToFrames(size ? *size : -1, m_Impl->decodeCapacityFrames, frames)) {
        m_Impl->fail("Invalid decoder buffer size", E_INVALIDARG);
        if (size) *size = 0;
    }
    // Keep a valid staging buffer even after worker failure: Session reaches
    // submitAudio(false), which triggers its existing renderer recovery path.
    return m_Impl->staging.data();
}

bool SpatialAudioRenderer::submitAudio(int bytesWritten)
{
    // Check failure BEFORE bytesWritten==0, including decode errors/missing data.
    if (m_Impl->failed.load() || !m_Impl->workerCreated) return false;
    std::size_t frames = 0;
    if (!Spatial71::bytesToFrames(bytesWritten, m_Impl->decodeCapacityFrames, frames)) {
        m_Impl->fail("Invalid decoded packet size", E_INVALIDARG);
        return false;
    }
    if (frames) {
        std::lock_guard<std::mutex> lock(m_Impl->queueMutex);
        if (!m_Impl->primed && !m_Impl->queue.size()) m_Impl->firstQueued = qpc();
        m_Impl->queue.push(m_Impl->staging.data(), frames);
    }
    return !m_Impl->failed.load();
}

IAudioRenderer::AudioFormat SpatialAudioRenderer::getAudioBufferFormat()
{
    return AudioFormat::Float32NE;
}
