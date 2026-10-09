#include "d3d11composition.h"

using Microsoft::WRL::ComPtr;

namespace {
uint64_t presentationTime100ns()
{
    static const uint64_t frequency = [] {
        LARGE_INTEGER value;
        return QueryPerformanceFrequency(&value) && value.QuadPart > 0 ?
            static_cast<uint64_t>(value.QuadPart) : uint64_t(0);
    }();
    LARGE_INTEGER now;
    return QueryPerformanceCounter(&now) && now.QuadPart > 0 ?
        PresentationClockSample::qpcTo100ns(static_cast<uint64_t>(now.QuadPart), frequency) : 0;
}
}

bool D3D11CompositionPresenter::runtimeSupported()
{
    static const bool supported = [] {
        if (!presentationTime100ns()) return false;
        // RtlGetVersion is independent of application-manifest version shims.
        using GetVersion = LONG (WINAPI*)(OSVERSIONINFOW*);
        const auto getVersion = reinterpret_cast<GetVersion>(
            GetProcAddress(GetModuleHandleW(L"ntdll.dll"), "RtlGetVersion"));
        OSVERSIONINFOW version = {};
        version.dwOSVersionInfoSize = sizeof(version);
        if (!getVersion || getVersion(&version) != 0 || version.dwMajorVersion < 10 ||
                version.dwBuildNumber < 22000) return false;
        if (version.dwBuildNumber > 22000) return true;
        DWORD revision = 0, size = sizeof(revision);
        return RegGetValueW(HKEY_LOCAL_MACHINE, L"SOFTWARE\\Microsoft\\Windows NT\\CurrentVersion",
            L"UBR", RRF_RT_REG_DWORD, nullptr, &revision, &size) == ERROR_SUCCESS && revision >= 194;
    }();
    return supported;
}

bool D3D11CompositionPresenter::deviceSupported(ID3D11Device* device)
{
    if (!runtimeSupported()) return false;
    HMODULE module = LoadLibraryExW(L"dcomp.dll", nullptr, LOAD_LIBRARY_SEARCH_SYSTEM32);
    if (!module) return false;
    using CreateFactory = HRESULT (WINAPI*)(IUnknown*, REFIID, void**);
    const auto createFactory = reinterpret_cast<CreateFactory>(GetProcAddress(module, "CreatePresentationFactory"));
    bool supported = false;
    {
        ComPtr<IPresentationFactory> factory;
        supported = createFactory && SUCCEEDED(createFactory(device, IID_PPV_ARGS(&factory))) &&
            factory->IsPresentationSupportedWithIndependentFlip();
    }
    FreeLibrary(module);
    return supported;
}

D3D11CompositionPresenter::~D3D11CompositionPresenter()
{
    reset();
}

void D3D11CompositionPresenter::reset()
{
    if (m_Visual) {
        m_Visual->SetContent(nullptr);
        m_Composition->Commit();
    }
    m_Visual.Reset();
    m_Target.Reset();
    m_Surface.Reset();
    if (m_Manager) {
        m_Manager->CancelPresentsFrom(1);
    }
    m_RetiringFence.Reset();
    m_Manager.Reset();
    m_Buffers = {};
    m_Ownership.reset();
    m_DisplayedFrames.clear();
    m_StatusFrames.clear();
    m_Composition.Reset();
    m_Device.Reset();
    if (m_LostEvent) CloseHandle(m_LostEvent);
    if (m_SurfaceHandle) CloseHandle(m_SurfaceHandle);
    if (m_Module) FreeLibrary(m_Module);
    m_LostEvent = nullptr;
    m_SurfaceHandle = nullptr;
    m_Module = nullptr;
    m_Acquired = m_Buffers.size();
    m_NextBuffer = 0;
    m_LastDisplayedId = 0;
    m_LastObservedDisplayId = m_RetiringPresentId = 0;
    m_ComposedFrames = m_IndependentFrames = 0;
    m_RejectedDisplayFrames = 0;
    m_QueuedFrames = m_SkippedFrames = m_CanceledFrames = 0;
    m_DroppedStatusFrames = 0;
}

HRESULT D3D11CompositionPresenter::initialize(ID3D11Device* device, HWND window,
                                             UINT width, UINT height, DXGI_FORMAT format,
                                             LUID adapter, UINT sourceId)
{
    reset();
    if (!runtimeSupported()) return DXGI_ERROR_UNSUPPORTED;
    m_Module = LoadLibraryExW(L"dcomp.dll", nullptr, LOAD_LIBRARY_SEARCH_SYSTEM32);
    if (!m_Module) return HRESULT_FROM_WIN32(GetLastError());
    using CreateFactory = HRESULT (WINAPI*)(IUnknown*, REFIID, void**);
    const auto createFactory = reinterpret_cast<CreateFactory>(
        GetProcAddress(m_Module, "CreatePresentationFactory"));
    if (!createFactory) { reset(); return E_NOINTERFACE; }

    // Keep failure cleanup in one place. No UI commit occurs until all buffers,
    // statistics, and the independent-flip capability have been initialized.
    const HRESULT result = [&]() -> HRESULT {
        ComPtr<IPresentationFactory> factory;
        HRESULT hr = createFactory(device, IID_PPV_ARGS(&factory));
        if (FAILED(hr)) return hr;
        if (!factory->IsPresentationSupportedWithIndependentFlip()) return DXGI_ERROR_UNSUPPORTED;
        m_Device = device;
        m_Adapter = adapter;
        m_SourceId = sourceId;
        hr = factory->CreatePresentationManager(&m_Manager);
        if (FAILED(hr)) return hr;
        hr = m_Manager->GetLostEvent(&m_LostEvent);
        if (FAILED(hr)) return hr;
        hr = m_Manager->GetPresentRetiringFence(IID_PPV_ARGS(&m_RetiringFence));
        if (FAILED(hr)) return hr;
        hr = m_Manager->EnablePresentStatisticsKind(PresentStatisticsKind_PresentStatus, TRUE);
        if (FAILED(hr)) return hr;
        hr = m_Manager->EnablePresentStatisticsKind(PresentStatisticsKind_IndependentFlipFrame, TRUE);
        if (FAILED(hr)) return hr;
        hr = m_Manager->EnablePresentStatisticsKind(PresentStatisticsKind_CompositionFrame, TRUE);
        if (FAILED(hr)) return hr;
        // Request prompt notification even with hardware flip queues. This is
        // an interrupt policy, not a wait or an extra VSync presentation delay.
        hr = m_Manager->ForceVSyncInterrupt(TRUE);
        if (FAILED(hr)) return hr;
        hr = DCompositionCreateSurfaceHandle(COMPOSITIONOBJECT_ALL_ACCESS, nullptr, &m_SurfaceHandle);
        if (FAILED(hr)) return hr;
        hr = m_Manager->CreatePresentationSurface(m_SurfaceHandle, &m_Surface);
        if (FAILED(hr)) return hr;
        m_Surface->SetTag(reinterpret_cast<UINT_PTR>(this));
        hr = m_Surface->SetAlphaMode(DXGI_ALPHA_MODE_IGNORE);
        if (FAILED(hr)) return hr;
        hr = m_Surface->SetColorSpace(DXGI_COLOR_SPACE_RGB_FULL_G22_NONE_P709);
        if (FAILED(hr)) return hr;
        hr = resize(width, height, format);
        if (FAILED(hr)) return hr;
        ComPtr<IDXGIDevice> dxgiDevice;
        hr = device->QueryInterface(IID_PPV_ARGS(&dxgiDevice));
        if (FAILED(hr)) return hr;
        hr = DCompositionCreateDevice(dxgiDevice.Get(), IID_PPV_ARGS(&m_Composition));
        if (FAILED(hr)) return hr;
        hr = m_Composition->CreateTargetForHwnd(window, TRUE, &m_Target);
        if (FAILED(hr)) return hr;
        hr = m_Composition->CreateVisual(&m_Visual);
        if (FAILED(hr)) return hr;
        ComPtr<IUnknown> content;
        hr = m_Composition->CreateSurfaceFromHandle(m_SurfaceHandle, &content);
        if (FAILED(hr)) return hr;
        hr = m_Visual->SetContent(content.Get());
        if (FAILED(hr)) return hr;
        hr = m_Target->SetRoot(m_Visual.Get());
        if (FAILED(hr)) return hr;
        return m_Composition->Commit();
    }();
    if (FAILED(result)) reset();
    return result;
}

HRESULT D3D11CompositionPresenter::resize(UINT width, UINT height, DXGI_FORMAT format)
{
    if (!m_Manager || !width || !height) return E_INVALIDARG;
    std::array<Buffer, 5> buffers;
    for (auto& buffer : buffers) {
        D3D11_TEXTURE2D_DESC desc = {};
        desc.Width = width;
        desc.Height = height;
        desc.MipLevels = desc.ArraySize = 1;
        desc.Format = format;
        desc.SampleDesc.Count = 1;
        desc.Usage = D3D11_USAGE_DEFAULT;
        desc.BindFlags = D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE;
        desc.MiscFlags = D3D11_RESOURCE_MISC_SHARED | D3D11_RESOURCE_MISC_SHARED_NTHANDLE |
            D3D11_RESOURCE_MISC_SHARED_DISPLAYABLE;
        HRESULT hr = m_Device->CreateTexture2D(&desc, nullptr, &buffer.texture);
        if (FAILED(hr)) return hr;
        hr = m_Device->CreateRenderTargetView(buffer.texture.Get(), nullptr, &buffer.view);
        if (FAILED(hr)) return hr;
        hr = m_Manager->AddBufferFromResource(buffer.texture.Get(), &buffer.presentation);
        if (FAILED(hr)) return hr;
    }
    const HRESULT hr = clearQueuedPresents();
    if (FAILED(hr)) return hr;
    // SetBuffer does not establish a sampling area. An unset source rectangle
    // produces successful presents with no visible content. Update it on every
    // resize as well as initial allocation so the whole rendered image is shown.
    const RECT sourceRect = {0, 0, static_cast<LONG>(width), static_cast<LONG>(height)};
    const HRESULT layoutResult = m_Surface->SetSourceRect(&sourceRect);
    if (FAILED(layoutResult)) return layoutResult;
    m_Buffers = std::move(buffers);
    m_Ownership.reset();
    cancel();
    m_NextBuffer = 0;
    return S_OK;
}

HRESULT D3D11CompositionPresenter::acquire(ID3D11RenderTargetView** view)
{
    if (!view) return E_POINTER;
    *view = nullptr;
    cancel();
    if (!m_Manager || WaitForSingleObject(m_LostEvent, 0) != WAIT_TIMEOUT) return DXGI_ERROR_DEVICE_REMOVED;
    drainStatistics();
    const HRESULT ownershipResult = refreshOwnership();
    if (FAILED(ownershipResult)) return ownershipResult;
    // Never wait for display retirement on the decode/prepare path. Returning
    // S_FALSE lets the bounded pacer discard stale work during an overload.
    for (size_t i = 0; i < m_Buffers.size(); ++i) {
        const size_t index = (m_NextBuffer + i) % m_Buffers.size();
        boolean available = FALSE;
        const HRESULT hr = m_Buffers[index].presentation->IsAvailable(&available);
        if (FAILED(hr)) return hr;
        if (available) {
            m_Ownership.available(index);
            m_Acquired = index;
            m_NextBuffer = (index + 1) % m_Buffers.size();
            return m_Buffers[index].view.CopyTo(view);
        }
    }
    return S_FALSE;
}

HRESULT D3D11CompositionPresenter::clearQueuedPresents()
{
    cancel();
    if (!m_Manager) return S_OK;
    drainStatistics();
    const HRESULT hr = m_Manager->CancelPresentsFrom(1);
    if (FAILED(hr)) return hr;
    // Canceled/skipped presents do not signal the retiring fence. Native buffer
    // availability is authoritative after cancellation; keep any displayed or
    // retiring buffer owned until the native API releases it.
    return refreshOwnership();
}

HRESULT D3D11CompositionPresenter::refreshOwnership()
{
    if (!m_RetiringFence) return E_UNEXPECTED;
    const uint64_t retired = m_RetiringFence->GetCompletedValue();
    if (retired == (std::numeric_limits<uint64_t>::max)()) return DXGI_ERROR_DEVICE_REMOVED;
    m_RetiringPresentId = retired;
    m_Ownership.retireThrough(retired);
    for (size_t i = 0; i < m_Buffers.size(); ++i) {
        if (!m_Buffers[i].presentation) continue;
        boolean available = FALSE;
        const HRESULT hr = m_Buffers[i].presentation->IsAvailable(&available);
        if (FAILED(hr)) return hr;
        if (available) m_Ownership.available(i);
    }
    return S_OK;
}

HRESULT D3D11CompositionPresenter::setColorSpace(DXGI_COLOR_SPACE_TYPE colorSpace)
{
    return m_Surface ? m_Surface->SetColorSpace(colorSpace) : E_UNEXPECTED;
}

HRESULT D3D11CompositionPresenter::present(uint64_t& id)
{
    return present(0, nullptr, id);
}

HRESULT D3D11CompositionPresenter::present(uint64_t targetUs, uint64_t (*clockUs)(),
                                          uint64_t& id, PresentResult* result)
{
    id = 0;
    if (result) *result = {};
    if (!m_Manager || m_Acquired == m_Buffers.size()) return E_UNEXPECTED;
    if (WaitForSingleObject(m_LostEvent, 0) != WAIT_TIMEOUT) return DXGI_ERROR_DEVICE_REMOVED;
    drainStatistics();
    HRESULT hr = refreshOwnership();
    if (FAILED(hr)) return hr;
    PresentResult observation;
    observation.requestedTargetUs = targetUs;
    if (targetUs) {
        if (!clockUs) return E_INVALIDARG;
        bool correlated = false;
        for (unsigned attempt = 0; attempt < 3; ++attempt) {
            observation.clockBeforeUs = clockUs();
            observation.referenceTime100ns = presentationTime100ns();
            observation.clockAfterUs = clockUs();
            if (!observation.referenceTime100ns || !observation.clockBeforeUs ||
                    observation.clockAfterUs < observation.clockBeforeUs ||
                    observation.clockAfterUs - observation.clockBeforeUs > 500) {
                continue;
            }
            const auto target = CompositionPresentTarget::translate(targetUs,
                observation.referenceTime100ns, observation.clockBeforeUs, observation.clockAfterUs);
            if (!target.time100ns) {
                cancel();
                return E_INVALIDARG; // A valid clock bracket leaves only target overflow.
            }
            observation.targetTime100ns = target.time100ns;
            observation.clockUncertaintyUs = target.uncertaintyUs;
            correlated = true;
            break;
        }
        if (!correlated) {
            // Preemption can widen an otherwise healthy correlation bracket.
            // Drop this image rather than invent a target or reset the device.
            cancel();
            return S_FALSE;
        }
    }
    else {
        observation.targetTime100ns = presentationTime100ns();
        if (!observation.targetTime100ns) return E_FAIL;
    }
    // Preserve distinct logical display deadlines when submitting prepared work
    // early. Windows may skip superseded/late updates; accepted is not displayed.
    hr = m_Manager->SetTargetTime({observation.targetTime100ns});
    if (FAILED(hr)) return hr;
    hr = m_Surface->SetBuffer(m_Buffers[m_Acquired].presentation.Get());
    if (FAILED(hr)) return hr;
    const uint64_t nextId = m_Manager->GetNextPresentId();
    const size_t acquired = m_Acquired;
    // Keep the actual API enqueue boundary distinct from clock correlation,
    // ownership queries, and feedback processing performed by this wrapper.
    observation.submissionTimeUs = clockUs ? clockUs() : 0;
    hr = m_Manager->Present();
    observation.submissionEndUs = clockUs ? clockUs() : 0;
    cancel();
    if (hr == S_OK) {
        id = nextId;
        m_Ownership.accept(acquired, nextId);
        observation.outstandingPresents = m_Ownership.outstanding();
        if (result) *result = observation;
    }
    return hr;
}

void D3D11CompositionPresenter::drainStatistics()
{
    if (!m_Manager) return;
    // Bound CPU work even if the OS delivers a batch after a suspension.
    for (unsigned i = 0; i < 64; ++i) {
        // Preserve every display event we already drained. If the caller is
        // not polling feedback, leave further records with the native queue
        // rather than grow storage indefinitely or silently overwrite events.
        if (m_DisplayedFrames.size() >= 1024) return;
        ComPtr<IPresentStatistics> statistics;
        if (m_Manager->GetNextPresentStatistics(&statistics) != S_OK || !statistics) return;
        if (statistics->GetKind() == PresentStatisticsKind_PresentStatus) {
            ComPtr<IPresentStatusPresentStatistics> status;
            if (FAILED(statistics.As(&status))) continue;
            const PresentStatusFrame frame = {status->GetPresentId(), status->GetPresentStatus()};
            switch (frame.status) {
            case PresentStatus_Queued:
                ++m_QueuedFrames;
                break;
            case PresentStatus_Skipped:
                ++m_SkippedFrames;
                m_Ownership.terminal(frame.id);
                break;
            case PresentStatus_Canceled:
                ++m_CanceledFrames;
                m_Ownership.terminal(frame.id);
                break;
            }
            // The caller normally consumes these every presentation. Bound the
            // legacy/probe path too, and expose any evidence lost to overflow.
            if (m_StatusFrames.size() == 1024) {
                m_StatusFrames.pop_front();
                ++m_DroppedStatusFrames;
            }
            m_StatusFrames.push_back(frame);
            continue;
        }
        if (statistics->GetKind() == PresentStatisticsKind_CompositionFrame) {
            ++m_ComposedFrames;
            continue;
        }
        if (statistics->GetKind() != PresentStatisticsKind_IndependentFlipFrame) continue;
        ++m_IndependentFrames;
        ComPtr<IIndependentFlipFramePresentStatistics> displayed;
        if (FAILED(statistics.As(&displayed))) continue;
        const auto adapter = displayed->GetOutputAdapterLUID();
        if (adapter.LowPart != m_Adapter.LowPart || adapter.HighPart != m_Adapter.HighPart ||
                displayed->GetOutputVidPnSourceId() != m_SourceId ||
                displayed->GetContentTag() != reinterpret_cast<UINT_PTR>(this) ||
                displayed->GetPresentId() <= m_LastObservedDisplayId) continue;
        m_LastObservedDisplayId = displayed->GetPresentId();
        m_DisplayedFrames.push_back({displayed->GetPresentId(),
            displayed->GetDisplayedTime().value, displayed->GetPresentDuration().value});
    }
}

bool D3D11CompositionPresenter::pollPresentStatus(PresentStatusFrame& frame)
{
    frame = {};
    drainStatistics();
    if (m_StatusFrames.empty()) return false;
    frame = m_StatusFrames.front();
    m_StatusFrames.pop_front();
    return true;
}

bool D3D11CompositionPresenter::pollDisplayedFrame(uint64_t (*clockUs)(), DisplayedFrame& frame)
{
    frame = {};
    if (!m_Manager || !clockUs) return false;
    drainStatistics();
    for (unsigned i = 0; i < 64 && !m_DisplayedFrames.empty(); ++i) {
        const auto displayed = m_DisplayedFrames.front();
        m_DisplayedFrames.pop_front();
        if (displayed.id <= m_LastDisplayedId) continue;
        const auto beforeUs = clockUs();
        const uint64_t referenceTime = presentationTime100ns();
        const auto afterUs = clockUs();
        const auto clock = PresentationClockSample::translate(
            displayed.time100ns, referenceTime, beforeUs, afterUs);
        if (!clock.timeUs) { ++m_RejectedDisplayFrames; continue; }
        m_LastDisplayedId = displayed.id;
        frame = {m_LastDisplayedId, clock, displayed.duration100ns};
        return true;
    }
    return false;
}
