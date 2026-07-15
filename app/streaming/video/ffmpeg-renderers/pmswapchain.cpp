#include "pmswapchain.h"

#include <Limelight.h>
#include <SDL.h>

#include <QtGlobal>

#include <cmath>

using Microsoft::WRL::ComPtr;

PmSwapchain::PmSwapchain()
    : m_DcompModule(nullptr),
      m_pCreatePresentationFactory(nullptr),
      m_pQueryInterruptTimePrecise(nullptr),
      m_AvailableEvents{},
      m_LostEvent(nullptr),
      m_StatsEvent(nullptr),
      m_CompSurfaceHandle(nullptr),
      m_BufferCount(3),
      m_CurrentBuffer(0),
      m_Lost(false),
      m_LastExpectedFlipUs(0),
      m_AcquireWaitUs(0),
      m_StatAcquireWaitUs(0),
      m_Pending{},
      m_PendingHead(0),
      m_ServoEnabled(true),
      m_ForceAsap(false),
      m_CompensationUs(0),
      m_ServoSamples(0),
      m_LastPresentCallUs(0),
      m_StatsStartUs(0),
      m_StatPresents(0),
      m_StatIflip(0),
      m_StatIflipAsap(0),
      m_StatMpoScanout(0),
      m_StatComposed(0),
      m_StatSkipped(0),
      m_StatCanceled(0),
      m_StatErrSamples(0),
      m_StatErrLateOverMs(0),
      m_StatErrSum(0),
      m_StatErrSumSq(0),
      m_StatErrMin(0),
      m_StatErrMax(0),
      m_StatAsapLatSamples(0),
      m_StatAsapLatSum(0),
      m_StatAsapLatSumSq(0),
      m_LastDisplayedTime(0),
      m_StatGlassSamples(0),
      m_StatGlassSum(0),
      m_StatGlassSumSq(0),
      m_LoggedFirstIflip(false),
      m_LoggedFirstComposed(false)
{
}

PmSwapchain::~PmSwapchain()
{
    for (int i = 0; i < NUM_BUFFERS; i++) {
        if (m_AvailableEvents[i] != nullptr) {
            CloseHandle(m_AvailableEvents[i]);
        }
    }
    if (m_LostEvent != nullptr) {
        CloseHandle(m_LostEvent);
    }
    if (m_StatsEvent != nullptr) {
        CloseHandle(m_StatsEvent);
    }
    if (m_CompSurfaceHandle != nullptr) {
        CloseHandle(m_CompSurfaceHandle);
    }
    if (m_DcompModule != nullptr) {
        FreeLibrary(m_DcompModule);
    }
}

bool PmSwapchain::initialize(ID3D11Device* device, HWND window,
                             int width, int height, DXGI_FORMAT format)
{
    HRESULT hr;

    // CreatePresentationFactory only exists in Windows 11's dcomp.dll; a
    // static import would keep the whole exe from loading on Windows 10.
    m_DcompModule = LoadLibraryExW(L"dcomp.dll", nullptr, LOAD_LIBRARY_SEARCH_SYSTEM32);
    if (m_DcompModule != nullptr) {
        m_pCreatePresentationFactory =
            (decltype(m_pCreatePresentationFactory))GetProcAddress(m_DcompModule,
                                                                   "CreatePresentationFactory");
    }
    if (m_pCreatePresentationFactory == nullptr) {
        SDL_LogError(SDL_LOG_CATEGORY_APPLICATION,
                     "PM: composition swapchain API unavailable (requires Windows 11 22000+)");
        return false;
    }

    m_pQueryInterruptTimePrecise =
        (decltype(m_pQueryInterruptTimePrecise))GetProcAddress(GetModuleHandleW(L"kernelbase.dll"),
                                                               "QueryInterruptTimePrecise");
    if (m_pQueryInterruptTimePrecise == nullptr) {
        SDL_LogError(SDL_LOG_CATEGORY_APPLICATION,
                     "PM: QueryInterruptTimePrecise unavailable");
        return false;
    }

    // Displayable-surface support decides whether presents can ever reach
    // direct scanout / independent flip (WDDM 3.0 allocation path).
    D3D11_FEATURE_DATA_DISPLAYABLE displayable = {};
    hr = device->CheckFeatureSupport(D3D11_FEATURE_DISPLAYABLE,
                                     &displayable, sizeof(displayable));
    if (FAILED(hr)) {
        displayable.DisplayableTexture = FALSE;
    }

    ComPtr<IPresentationFactory> factory;
    hr = m_pCreatePresentationFactory(device, IID_PPV_ARGS(&factory));
    if (FAILED(hr)) {
        SDL_LogError(SDL_LOG_CATEGORY_APPLICATION,
                     "PM: CreatePresentationFactory() failed: %x", hr);
        return false;
    }

    bool supported = factory->IsPresentationSupported();
    bool iflipSupported = factory->IsPresentationSupportedWithIndependentFlip();
    SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION,
                "PM: support=%d iflipSupport=%d displayableTexture=%d",
                supported, iflipSupported, displayable.DisplayableTexture);

    if (!supported) {
        SDL_LogError(SDL_LOG_CATEGORY_APPLICATION,
                     "PM: presentation manager not supported on this system");
        return false;
    }

    bool useDisplayable = iflipSupported && displayable.DisplayableTexture;
    if (!useDisplayable && qEnvironmentVariableIntValue("MOONLIGHT_PM_ALLOW_COMPOSED") == 0) {
        // Composed-only PM presents go through DWM, which re-quantizes
        // timing to its own frame cadence - the experiment would measure
        // DWM, not the target-time executor. Require iflip eligibility
        // unless the user explicitly wants the composed data point.
        SDL_LogError(SDL_LOG_CATEGORY_APPLICATION,
                     "PM: no independent flip path (WDDM 3.0 displayable surfaces required); "
                     "set MOONLIGHT_PM_ALLOW_COMPOSED=1 to run composed anyway");
        return false;
    }

    hr = factory->CreatePresentationManager(&m_Manager);
    if (FAILED(hr)) {
        SDL_LogError(SDL_LOG_CATEGORY_APPLICATION,
                     "PM: CreatePresentationManager() failed: %x", hr);
        return false;
    }

    int bufferCountOverride = qEnvironmentVariableIntValue("MOONLIGHT_PM_BUFFERS");
    if (bufferCountOverride != 0) {
        if (bufferCountOverride >= 2 && bufferCountOverride <= NUM_BUFFERS) {
            m_BufferCount = bufferCountOverride;
            SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION,
                        "PM: buffer ring count overridden to %d", m_BufferCount);
        }
        else {
            SDL_LogWarn(SDL_LOG_CATEGORY_APPLICATION,
                        "PM: ignoring MOONLIGHT_PM_BUFFERS=%d (valid range 2-%d)",
                        bufferCountOverride, NUM_BUFFERS);
        }
    }

    // Buffer ring. Displayable allocation is what makes iflip/MPO promotion
    // possible for these textures.
    for (int i = 0; i < m_BufferCount; i++) {
        D3D11_TEXTURE2D_DESC texDesc = {};
        texDesc.Width = width;
        texDesc.Height = height;
        texDesc.MipLevels = 1;
        texDesc.ArraySize = 1;
        texDesc.Format = format;
        texDesc.SampleDesc.Count = 1;
        texDesc.SampleDesc.Quality = 0;
        texDesc.Usage = D3D11_USAGE_DEFAULT;
        texDesc.BindFlags = D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE;
        texDesc.CPUAccessFlags = 0;
        texDesc.MiscFlags = D3D11_RESOURCE_MISC_SHARED | D3D11_RESOURCE_MISC_SHARED_NTHANDLE;
        if (useDisplayable) {
            texDesc.MiscFlags |= D3D11_RESOURCE_MISC_SHARED_DISPLAYABLE;
        }

        hr = device->CreateTexture2D(&texDesc, nullptr, &m_Textures[i]);
        if (FAILED(hr)) {
            SDL_LogError(SDL_LOG_CATEGORY_APPLICATION,
                         "PM: CreateTexture2D(buffer %d%s) failed: %x",
                         i, useDisplayable ? ", displayable" : "", hr);
            return false;
        }

        hr = device->CreateRenderTargetView(m_Textures[i].Get(), nullptr, &m_Rtvs[i]);
        if (FAILED(hr)) {
            SDL_LogError(SDL_LOG_CATEGORY_APPLICATION,
                         "PM: CreateRenderTargetView(buffer %d) failed: %x", i, hr);
            return false;
        }

        hr = m_Manager->AddBufferFromResource(m_Textures[i].Get(), &m_Buffers[i]);
        if (FAILED(hr)) {
            SDL_LogError(SDL_LOG_CATEGORY_APPLICATION,
                         "PM: AddBufferFromResource(buffer %d) failed: %x", i, hr);
            return false;
        }

        hr = m_Buffers[i]->GetAvailableEvent(&m_AvailableEvents[i]);
        if (FAILED(hr)) {
            SDL_LogError(SDL_LOG_CATEGORY_APPLICATION,
                         "PM: GetAvailableEvent(buffer %d) failed: %x", i, hr);
            return false;
        }
    }

    // Presentation surface bound into a DirectComposition tree over the
    // SDL window - this replaces the DXGI swapchain's HWND binding.
    hr = DCompositionCreateSurfaceHandle(COMPOSITIONOBJECT_ALL_ACCESS, nullptr,
                                         &m_CompSurfaceHandle);
    if (FAILED(hr)) {
        SDL_LogError(SDL_LOG_CATEGORY_APPLICATION,
                     "PM: DCompositionCreateSurfaceHandle() failed: %x", hr);
        return false;
    }

    hr = m_Manager->CreatePresentationSurface(m_CompSurfaceHandle, &m_Surface);
    if (FAILED(hr)) {
        SDL_LogError(SDL_LOG_CATEGORY_APPLICATION,
                     "PM: CreatePresentationSurface() failed: %x", hr);
        return false;
    }

    m_Surface->SetAlphaMode(DXGI_ALPHA_MODE_IGNORE);
    RECT sourceRect = { 0, 0, width, height };
    m_Surface->SetSourceRect(&sourceRect);

    ComPtr<IDXGIDevice> dxgiDevice;
    hr = device->QueryInterface(IID_PPV_ARGS(&dxgiDevice));
    if (FAILED(hr)) {
        SDL_LogError(SDL_LOG_CATEGORY_APPLICATION,
                     "PM: ID3D11Device::QueryInterface(IDXGIDevice) failed: %x", hr);
        return false;
    }

    hr = DCompositionCreateDevice(dxgiDevice.Get(), IID_PPV_ARGS(&m_DcompDevice));
    if (FAILED(hr)) {
        SDL_LogError(SDL_LOG_CATEGORY_APPLICATION,
                     "PM: DCompositionCreateDevice() failed: %x", hr);
        return false;
    }

    hr = m_DcompDevice->CreateTargetForHwnd(window, TRUE, &m_DcompTarget);
    if (FAILED(hr)) {
        SDL_LogError(SDL_LOG_CATEGORY_APPLICATION,
                     "PM: IDCompositionDevice::CreateTargetForHwnd() failed: %x", hr);
        return false;
    }

    hr = m_DcompDevice->CreateVisual(&m_DcompVisual);
    if (FAILED(hr)) {
        SDL_LogError(SDL_LOG_CATEGORY_APPLICATION,
                     "PM: IDCompositionDevice::CreateVisual() failed: %x", hr);
        return false;
    }

    hr = m_DcompDevice->CreateSurfaceFromHandle(m_CompSurfaceHandle, &m_DcompContent);
    if (FAILED(hr)) {
        SDL_LogError(SDL_LOG_CATEGORY_APPLICATION,
                     "PM: IDCompositionDevice::CreateSurfaceFromHandle() failed: %x", hr);
        return false;
    }

    m_DcompVisual->SetContent(m_DcompContent.Get());
    m_DcompTarget->SetRoot(m_DcompVisual.Get());
    hr = m_DcompDevice->Commit();
    if (FAILED(hr)) {
        SDL_LogError(SDL_LOG_CATEGORY_APPLICATION,
                     "PM: IDCompositionDevice::Commit() failed: %x", hr);
        return false;
    }

    m_Manager->GetLostEvent(&m_LostEvent);

    m_Manager->EnablePresentStatisticsKind(PresentStatisticsKind_PresentStatus, true);
    m_Manager->EnablePresentStatisticsKind(PresentStatisticsKind_CompositionFrame, true);
    m_Manager->EnablePresentStatisticsKind(PresentStatisticsKind_IndependentFlipFrame, true);
    m_Manager->GetPresentStatisticsAvailableEvent(&m_StatsEvent);

    // Without DWM in the loop, target-time evaluation can be deferred to
    // whenever a vsync interrupt happens to fire; keep the interrupt on so
    // presents are considered promptly (candidate cause of the measured
    // ~20ms constant lateness on the first field test).
    if (qEnvironmentVariableIntValue("MOONLIGHT_PM_NO_FORCE_VSYNC_INT") == 0) {
        hr = m_Manager->ForceVSyncInterrupt(TRUE);
        SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION,
                    "PM: ForceVSyncInterrupt(TRUE): %x", hr);
    }

    if (qEnvironmentVariableIntValue("MOONLIGHT_PM_NO_OFFSET_SERVO") != 0) {
        m_ServoEnabled = false;
        SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION,
                    "PM: offset servo disabled by environment");
    }

    // Deliberate FIFO mode: no target times at all; the OS present queue
    // does the de-jittering and latency is whatever the pipeline floor is.
    // The v2 field test ran this mode by accident (servo windup) and it
    // subjectively beat scheduled mode on latency - this makes the A/B
    // intentional.
    if (qEnvironmentVariableIntValue("MOONLIGHT_PM_ASAP") != 0) {
        m_ForceAsap = true;
        SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION,
                    "PM: forced ASAP (FIFO) mode - pacer targets not sent to the OS");
    }

    SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION,
                "PM: composition swapchain active: %dx%d format=%d buffers=%d displayable=%d",
                width, height, format, m_BufferCount, useDisplayable);
    return true;
}

ID3D11RenderTargetView* PmSwapchain::acquireBuffer()
{
    // Lost event first so device loss preempts a buffer wait.
    HANDLE waitHandles[1 + NUM_BUFFERS];
    waitHandles[0] = m_LostEvent;
    for (int i = 0; i < m_BufferCount; i++) {
        waitHandles[i + 1] = m_AvailableEvents[i];
    }

    uint64_t startUs = LiGetMicroseconds();
    DWORD result = WaitForMultipleObjects(1 + m_BufferCount, waitHandles, FALSE, 200);
    uint64_t waitedUs = LiGetMicroseconds() - startUs;
    m_AcquireWaitUs += waitedUs;
    m_StatAcquireWaitUs += waitedUs;

    if (result == WAIT_OBJECT_0) {
        SDL_LogError(SDL_LOG_CATEGORY_APPLICATION,
                     "PM: presentation manager lost");
        m_Lost = true;
        return nullptr;
    }

    if (result < WAIT_OBJECT_0 + 1 || result > WAIT_OBJECT_0 + (DWORD)m_BufferCount) {
        SDL_LogWarn(SDL_LOG_CATEGORY_APPLICATION,
                    "PM: no presentation buffer available (wait result %lu)",
                    result);
        return nullptr;
    }

    m_CurrentBuffer = (int)(result - WAIT_OBJECT_0 - 1);
    return m_Rtvs[m_CurrentBuffer].Get();
}

void PmSwapchain::setColorSpace(DXGI_COLOR_SPACE_TYPE colorSpace, bool isHdr)
{
    HRESULT hr = m_Surface->SetColorSpace(colorSpace);
    if (FAILED(hr)) {
        SDL_LogError(SDL_LOG_CATEGORY_APPLICATION,
                     "PM: IPresentationSurface::SetColorSpace(%d) failed: %x",
                     colorSpace, hr);
    }

    ComPtr<IPresentationSurface2> surface2;
    if (SUCCEEDED(m_Surface.As(&surface2))) {
        surface2->SetIsHdrContent(isHdr);
    }
    else if (isHdr) {
        SDL_LogWarn(SDL_LOG_CATEGORY_APPLICATION,
                    "PM: IPresentationSurface2 unavailable; no SetIsHdrContent hint");
    }
}

bool PmSwapchain::present(uint64_t targetUs)
{
    uint64_t nowUs = LiGetMicroseconds();

    // Make long present-stream gaps (network stalls, host hitches) visible
    // in the log so downstream oddities can be correlated with them.
    if (m_LastPresentCallUs != 0 && nowUs - m_LastPresentCallUs > 250000ULL) {
        SDL_LogWarn(SDL_LOG_CATEGORY_APPLICATION,
                    "PM: present stream resumed after %llu ms gap",
                    (unsigned long long)((nowUs - m_LastPresentCallUs) / 1000));
    }
    m_LastPresentCallUs = nowUs;

    ULONGLONG nowInterrupt = 0;
    m_pQueryInterruptTimePrecise(&nowInterrupt);

    // Servo: ask earlier by the measured executor lateness so the frame
    // hits the glass nearer the pacer's wanted instant. Clamped per frame
    // so the sent target stays >= now+1ms: the executor delay proved to be
    // a pipeline FLOOR, and an unclamped servo winds up until every
    // present silently degenerates to ASAP (the v2 field test).
    uint64_t sentTargetUs = m_ForceAsap ? 0 : targetUs;
    if (!m_ForceAsap && m_ServoEnabled && targetUs > nowUs && m_ServoSamples >= 50) {
        uint64_t headroomUs = targetUs - nowUs;
        uint64_t applied = m_CompensationUs;
        if (applied + 1000 > headroomUs) {
            applied = headroomUs > 1000 ? headroomUs - 1000 : 0;
        }
        sentTargetUs = targetUs - applied;
    }

    SystemInterruptTime target = {};
    UINT64 wantedInterrupt = targetUs > nowUs ?
        nowInterrupt + (targetUs - nowUs) * 10 : 0;
    if (sentTargetUs > nowUs) {
        // Translate the pacer's QPC-domain target into interrupt time by
        // offsetting from a now-sample of both clocks.
        target.value = nowInterrupt + (sentTargetUs - nowUs) * 10;
        m_LastExpectedFlipUs = targetUs;
    }
    else {
        // 0 = show as soon as possible
        m_LastExpectedFlipUs = targetUs > nowUs ? targetUs : nowUs;
    }

    HRESULT hr = m_Manager->SetTargetTime(target);
    if (FAILED(hr)) {
        SDL_LogError(SDL_LOG_CATEGORY_APPLICATION,
                     "PM: SetTargetTime() failed: %x", hr);
    }

    hr = m_Surface->SetBuffer(m_Buffers[m_CurrentBuffer].Get());
    if (FAILED(hr)) {
        SDL_LogError(SDL_LOG_CATEGORY_APPLICATION,
                     "PM: SetBuffer() failed: %x", hr);
        return false;
    }

    UINT64 presentId = m_Manager->GetNextPresentId();

    hr = m_Manager->Present();
    if (hr == PRESENTATION_ERROR_LOST) {
        SDL_LogError(SDL_LOG_CATEGORY_APPLICATION,
                     "PM: Present() reported PRESENTATION_ERROR_LOST");
        m_Lost = true;
        return false;
    }
    else if (FAILED(hr)) {
        SDL_LogError(SDL_LOG_CATEGORY_APPLICATION,
                     "PM: Present() failed: %x", hr);
        return false;
    }

    m_Pending[m_PendingHead] = { presentId, target.value, wantedInterrupt,
                                 nowInterrupt };
    m_PendingHead = (m_PendingHead + 1) % PENDING_RING;

    m_StatPresents++;
    // Statistics can arrive in bursts. Drain them only after the target and
    // buffer are safely submitted so telemetry backlog cannot make this
    // frame miss its requested presentation time.
    drainStatistics();
    logStatsIfDue(nowUs);
    return true;
}

uint64_t PmSwapchain::popAcquireWaitUs()
{
    uint64_t waitUs = m_AcquireWaitUs;
    m_AcquireWaitUs = 0;
    return waitUs;
}

const PmSwapchain::PendingPresent* PmSwapchain::lookupPending(UINT64 presentId)
{
    for (int i = 0; i < PENDING_RING; i++) {
        if (m_Pending[i].presentId == presentId) {
            return &m_Pending[i];
        }
    }
    return nullptr;
}

void PmSwapchain::updateServo(int64_t netErrorUs)
{
    m_ServoSamples++;

    // Integrate toward a small positive guard (never ask for the past on
    // purpose); clamp so a bad sample burst can't fling targets 30ms early.
    constexpr int64_t GUARD_US = 2000;
    constexpr int64_t MAX_COMP_US = 22000;
    int64_t comp = (int64_t)m_CompensationUs;
    comp += (netErrorUs - GUARD_US) / 50;
    if (comp < 0) comp = 0;
    if (comp > MAX_COMP_US) comp = MAX_COMP_US;
    m_CompensationUs = (uint64_t)comp;
}

void PmSwapchain::recordTargetError(double errorMs)
{
    if (m_StatErrSamples == 0) {
        m_StatErrMin = m_StatErrMax = errorMs;
    }
    else {
        m_StatErrMin = qMin(m_StatErrMin, errorMs);
        m_StatErrMax = qMax(m_StatErrMax, errorMs);
    }
    m_StatErrSamples++;
    m_StatErrSum += errorMs;
    m_StatErrSumSq += errorMs * errorMs;
    if (errorMs > 1.0) {
        m_StatErrLateOverMs++;
    }
}

void PmSwapchain::drainStatistics()
{
    if (m_StatsEvent == nullptr) {
        return;
    }

    // Keep telemetry work bounded on the cadence thread. If more records are
    // queued, the still-signaled event lets the next present continue where
    // this one stopped.
    constexpr int MAX_STATS_PER_DRAIN = 32;
    int drained = 0;
    while (drained++ < MAX_STATS_PER_DRAIN &&
           WaitForSingleObject(m_StatsEvent, 0) == WAIT_OBJECT_0) {
        ComPtr<IPresentStatistics> stat;
        if (FAILED(m_Manager->GetNextPresentStatistics(&stat)) || !stat) {
            break;
        }

        switch (stat->GetKind()) {
        case PresentStatisticsKind_PresentStatus:
        {
            ComPtr<IPresentStatusPresentStatistics> status;
            if (SUCCEEDED(stat.As(&status))) {
                switch (status->GetPresentStatus()) {
                case PresentStatus_Queued:
                    break;
                case PresentStatus_Skipped:
                    m_StatSkipped++;
                    break;
                case PresentStatus_Canceled:
                    m_StatCanceled++;
                    break;
                }
            }
            break;
        }

        case PresentStatisticsKind_CompositionFrame:
        {
            // Presents that went through DWM. ScanoutOnScreen means an MPO
            // plane (direct scanout with DWM still owning the display);
            // ComposedOnScreen means a full DWM copy - the timing-hostile
            // case.
            ComPtr<ICompositionFramePresentStatistics> comp;
            if (SUCCEEDED(stat.As(&comp))) {
                UINT instanceCount = 0;
                const CompositionFrameDisplayInstance* instances = nullptr;
                comp->GetDisplayInstanceArray(&instanceCount, &instances);

                bool scanout = false;
                for (UINT i = 0; i < instanceCount; i++) {
                    if (instances[i].instanceKind == CompositionFrameInstanceKind_ScanoutOnScreen) {
                        scanout = true;
                    }
                }
                if (scanout) {
                    m_StatMpoScanout++;
                }
                else {
                    m_StatComposed++;
                }

                if (!m_LoggedFirstComposed) {
                    m_LoggedFirstComposed = true;
                    SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION,
                                "PM: presents are being %s by DWM (instances=%u)",
                                scanout ? "scanned out via MPO" : "COMPOSED (timing re-quantized!)",
                                instanceCount);
                }
            }
            break;
        }

        case PresentStatisticsKind_IndependentFlipFrame:
        {
            ComPtr<IIndependentFlipFramePresentStatistics> iflip;
            if (SUCCEEDED(stat.As(&iflip))) {
                m_StatIflip++;

                if (!m_LoggedFirstIflip) {
                    m_LoggedFirstIflip = true;
                    SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION,
                                "PM: presents are reaching INDEPENDENT FLIP");
                }

                UINT64 displayed = iflip->GetDisplayedTime().value;

                // Glass-interval jitter across every iflip frame: the
                // judder metric, comparable between scheduled and ASAP
                // modes. Deltas across stalls (>100ms) are cadence gaps,
                // not pacing error - skip them.
                if (m_LastDisplayedTime != 0 && displayed > m_LastDisplayedTime &&
                        displayed - m_LastDisplayedTime < 1000000ULL) {
                    double deltaMs = (displayed - m_LastDisplayedTime) / 10000.0;
                    m_StatGlassSamples++;
                    m_StatGlassSum += deltaMs;
                    m_StatGlassSumSq += deltaMs * deltaMs;
                }
                m_LastDisplayedTime = displayed;

                const PendingPresent* pending = lookupPending(stat->GetPresentId());
                if (pending != nullptr && pending->wantedTargetInterruptTime != 0) {
                    // Net error: displayed vs the pacer's WANTED instant
                    // (positive = late). This is what the user experiences;
                    // raw executor lateness = net + compensation.
                    int64_t netError100ns =
                        (int64_t)displayed -
                        (int64_t)pending->wantedTargetInterruptTime;
                    recordTargetError(netError100ns / 10000.0);

                    if (pending->sentTargetInterruptTime != 0) {
                        updateServo(netError100ns / 10);
                    }
                }
                if (pending == nullptr || pending->sentTargetInterruptTime == 0) {
                    m_StatIflipAsap++;
                    if (pending != nullptr &&
                            displayed > pending->presentCallInterruptTime) {
                        // True OS pipeline latency of a FIFO present.
                        double latMs =
                            (displayed - pending->presentCallInterruptTime) / 10000.0;
                        m_StatAsapLatSamples++;
                        m_StatAsapLatSum += latMs;
                        m_StatAsapLatSumSq += latMs * latMs;
                    }
                }
            }
            break;
        }
        }
    }
}

void PmSwapchain::logStatsIfDue(uint64_t nowUs)
{
    if (m_StatsStartUs == 0) {
        m_StatsStartUs = nowUs;
        return;
    }
    if (nowUs - m_StatsStartUs < 10000000ULL) {
        return;
    }

    double mean = m_StatErrSamples > 0 ? m_StatErrSum / m_StatErrSamples : 0.0;
    double variance = m_StatErrSamples > 0
        ? qMax(0.0, m_StatErrSumSq / m_StatErrSamples - mean * mean) : 0.0;
    double glassMean = m_StatGlassSamples > 0
        ? m_StatGlassSum / m_StatGlassSamples : 0.0;
    double glassVar = m_StatGlassSamples > 0
        ? qMax(0.0, m_StatGlassSumSq / m_StatGlassSamples - glassMean * glassMean) : 0.0;
    double asapMean = m_StatAsapLatSamples > 0
        ? m_StatAsapLatSum / m_StatAsapLatSamples : 0.0;
    double asapVar = m_StatAsapLatSamples > 0
        ? qMax(0.0, m_StatAsapLatSumSq / m_StatAsapLatSamples - asapMean * asapMean) : 0.0;

    SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION,
                "PM present stats: n=%u iflip=%u (asap=%u) mpo=%u composed=%u "
                "skipped=%u canceled=%u | net err vs wanted ms: n=%u mean=%.2f sd=%.2f "
                "min=%.2f max=%.2f late>1ms=%u | servo comp=%.1fms | "
                "glass dt ms: n=%u mean=%.2f sd=%.2f | asap pipeline ms: n=%u mean=%.2f sd=%.2f | "
                "acq wait avg=%.2fms",
                m_StatPresents, m_StatIflip, m_StatIflipAsap, m_StatMpoScanout,
                m_StatComposed, m_StatSkipped, m_StatCanceled,
                m_StatErrSamples, mean, sqrt(variance),
                m_StatErrMin, m_StatErrMax, m_StatErrLateOverMs,
                m_CompensationUs / 1000.0,
                m_StatGlassSamples, glassMean, sqrt(glassVar),
                m_StatAsapLatSamples, asapMean, sqrt(asapVar),
                m_StatPresents > 0 ? m_StatAcquireWaitUs / 1000.0 / m_StatPresents : 0.0);

    m_StatsStartUs = nowUs;
    m_StatPresents = m_StatIflip = m_StatIflipAsap = 0;
    m_StatMpoScanout = m_StatComposed = 0;
    m_StatSkipped = m_StatCanceled = 0;
    m_StatErrSamples = m_StatErrLateOverMs = 0;
    m_StatErrSum = m_StatErrSumSq = 0;
    m_StatErrMin = m_StatErrMax = 0;
    m_StatGlassSamples = 0;
    m_StatGlassSum = m_StatGlassSumSq = 0;
    m_StatAsapLatSamples = 0;
    m_StatAsapLatSum = m_StatAsapLatSumSq = 0;
    m_StatAcquireWaitUs = 0;
}
