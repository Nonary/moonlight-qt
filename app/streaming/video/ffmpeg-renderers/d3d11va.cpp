// For D3D11_DECODER_PROFILE values
#include <initguid.h>

#include "d3d11va.h"
#include "dxutil.h"
#include "path.h"
#include "utils.h"

#include "streaming/streamutils.h"
#include "streaming/session.h"

#include <SDL_syswm.h>

#include <dwmapi.h>

using Microsoft::WRL::ComPtr;

// Standard DXVA GUIDs for HEVC RExt profiles (redefined for compatibility with pre-24H2 SDKs)
DEFINE_GUID(k_D3D11_DECODER_PROFILE_HEVC_VLD_MAIN_444,   0x4008018f, 0xf537, 0x4b36, 0x98, 0xcf, 0x61, 0xaf, 0x8a, 0x2c, 0x1a, 0x33);
DEFINE_GUID(k_D3D11_DECODER_PROFILE_HEVC_VLD_MAIN10_444, 0x0dabeffa, 0x4458, 0x4602, 0xbc, 0x03, 0x07, 0x95, 0x65, 0x9d, 0x61, 0x7c);

typedef struct _VERTEX
{
    float x, y;
    float tu, tv;
} VERTEX, *PVERTEX;

#define CSC_MATRIX_RAW_ELEMENT_COUNT 9
#define CSC_MATRIX_PACKED_ELEMENT_COUNT 12
#define OFFSETS_ELEMENT_COUNT 3

typedef struct _CSC_CONST_BUF
{
    // CscMatrix value from above but packed and scaled
    float cscMatrix[CSC_MATRIX_PACKED_ELEMENT_COUNT];

    // YUV offset values
    float offsets[OFFSETS_ELEMENT_COUNT];

    // Padding float to end 16-byte boundary
    float padding;

    // Chroma offset values
    float chromaOffset[2];

    // Max UV coordinates to avoid sampling alignment padding
    float chromaUVMax[2];
} CSC_CONST_BUF, *PCSC_CONST_BUF;
static_assert(sizeof(CSC_CONST_BUF) % 16 == 0, "Constant buffer sizes must be a multiple of 16");

static const std::array<const char*, D3D11VARenderer::PixelShaders::_COUNT> k_VideoShaderNames =
{
    "d3d11_yuv420_pixel.fxc",
    "d3d11_ayuv_pixel.fxc",
    "d3d11_y410_pixel.fxc",
};

D3D11VARenderer::D3D11VARenderer(int decoderSelectionPass)
    : IFFmpegRenderer(RendererType::D3D11VA),
      m_DecoderSelectionPass(decoderSelectionPass),
      m_DevicesWithFL11Support(0),
      m_DevicesWithCodecSupport(0),
      m_LastColorTrc(AVCOL_TRC_UNSPECIFIED),
      m_AllowTearing(false),
      m_PresentationMode(PresentationMode::Auto),
      m_PresentationModeFallbackReason(nullptr),
      m_TearingSupport(false),
      m_OutputIndex(-1),
      m_KmtAdapterValid(false),
      m_KmtAdapter(0),
      m_KmtVidPnSourceId(0),
      m_ActiveScanLines(0),
      m_ScanoutPeriodUs(0),
      m_LastPresentAlignmentWaitUs(0),
      m_PresentTargetUs(0),
      m_PresentAlignBudgetUs(0),
      m_LastPresentUs(0),
      m_AlignHits(0),
      m_AlignGiveUps(0),
      m_AlignSkips(0),
      m_AlignWaitTotalUs(0),
      m_AlignBudgetTotalUs(0),
      m_AlignStatsStartUs(0),
      m_PresentReadyFenceValue(0),
      m_PresentReadyFenceEvent(nullptr),
      m_PresentReadyFenceFailed(false),
      m_FrameLatencyWaitableObject(nullptr),
      m_OverlayLock(0),
      m_HwDeviceContext(nullptr)
{
    m_ContextLock = SDL_CreateMutex();

    DwmEnableMMCSS(TRUE);
}

D3D11VARenderer::~D3D11VARenderer()
{
    DwmEnableMMCSS(FALSE);

    if (m_KmtAdapterValid) {
        D3DKMT_CLOSEADAPTER closeAdapter = {};
        closeAdapter.hAdapter = m_KmtAdapter;
        D3DKMTCloseAdapter(&closeAdapter);
        m_KmtAdapterValid = false;
    }

    SDL_DestroyMutex(m_ContextLock);

    m_VideoVertexBuffer.Reset();
    for (auto& shader : m_VideoPixelShaders) {
        shader.Reset();
    }

    for (auto& textureSrvs : m_VideoTextureResourceViews) {
        for (auto& srv : textureSrvs) {
            srv.Reset();
        }
    }

    m_VideoTexture.Reset();

    for (auto& buffer : m_OverlayVertexBuffers) {
        buffer.Reset();
    }

    for (auto& srv : m_OverlayTextureResourceViews) {
        srv.Reset();
    }

    for (auto& texture : m_OverlayTextures) {
        texture.Reset();
    }

    m_OverlayPixelShader.Reset();

    m_OverlayBlendState.Reset();
    m_VideoBlendState.Reset();

    m_DecodeD2RFence.Reset();
    m_DecodeR2DFence.Reset();
    m_RenderD2RFence.Reset();
    m_RenderR2DFence.Reset();

    m_PresentReadyFence.Reset();
    if (m_PresentReadyFenceEvent != nullptr) {
        CloseHandle(m_PresentReadyFenceEvent);
        m_PresentReadyFenceEvent = nullptr;
    }
    m_RenderTargetView.Reset();
    if (m_FrameLatencyWaitableObject != nullptr) {
        CloseHandle(m_FrameLatencyWaitableObject);
        m_FrameLatencyWaitableObject = nullptr;
    }
    m_SwapChain.Reset();

    m_RenderSharedTextureArray.Reset();

    av_buffer_unref(&m_HwDeviceContext);
    m_DecodeDevice.Reset();
    m_DecodeDeviceContext.Reset();

    // Force destruction of the swapchain immediately
    if (m_RenderDeviceContext != nullptr) {
        m_RenderDeviceContext->ClearState();
        m_RenderDeviceContext->Flush();
    }

    m_RenderDevice.Reset();
    m_RenderDeviceContext.Reset();
    m_Factory.Reset();
}

bool D3D11VARenderer::createSharedFencePair(UINT64 initialValue, ID3D11Device5* dev1, ID3D11Device5* dev2, ComPtr<ID3D11Fence>& dev1Fence, ComPtr<ID3D11Fence>& dev2Fence)
{
    HRESULT hr;
    D3D11_FENCE_FLAG flags;

    flags = D3D11_FENCE_FLAG_SHARED;
    if (m_FenceType == SupportedFenceType::NonMonitored) {
        flags |= D3D11_FENCE_FLAG_NON_MONITORED;
    }

    hr = dev1->CreateFence(initialValue, flags, IID_PPV_ARGS(&dev1Fence));
    if (FAILED(hr)) {
        SDL_LogError(SDL_LOG_CATEGORY_APPLICATION,
                     "ID3D11Device5::CreateFence() failed: %x",
                     hr);
        return false;
    }

    HANDLE fenceHandle;
    hr = dev1Fence->CreateSharedHandle(nullptr, GENERIC_ALL, nullptr, &fenceHandle);
    if (FAILED(hr)) {
        SDL_LogError(SDL_LOG_CATEGORY_APPLICATION,
                     "ID3D11Fence::CreateSharedHandle() failed: %x",
                     hr);
        dev1Fence.Reset();
        return false;
    }

    hr = dev2->OpenSharedFence(fenceHandle, IID_PPV_ARGS(&dev2Fence));
    CloseHandle(fenceHandle);
    if (FAILED(hr)) {
        SDL_LogError(SDL_LOG_CATEGORY_APPLICATION,
                     "ID3D11Device5::OpenSharedFence() failed: %x",
                     hr);
        dev1Fence.Reset();
        return false;
    }

    return true;
}

bool D3D11VARenderer::setupSharedDevice(IDXGIAdapter1* adapter)
{
    const D3D_FEATURE_LEVEL supportedFeatureLevels[] = { D3D_FEATURE_LEVEL_11_1, D3D_FEATURE_LEVEL_11_0 };
    D3D_FEATURE_LEVEL featureLevel;
    HRESULT hr;
    ComPtr<ID3D11Device> device;
    ComPtr<ID3D11DeviceContext> deviceContext;
    bool success = false;

    // We don't support cross-device sharing without fences
    if (m_FenceType == SupportedFenceType::None) {
        return false;
    }

    // If we're going to use separate devices for decoding and rendering, create the decoding device
    hr = D3D11CreateDevice(adapter,
                           D3D_DRIVER_TYPE_UNKNOWN,
                           nullptr,
                           D3D11_CREATE_DEVICE_VIDEO_SUPPORT
                               | (m_DebugLayer ? D3D11_CREATE_DEVICE_DEBUG : 0),
                           supportedFeatureLevels,
                           ARRAYSIZE(supportedFeatureLevels),
                           D3D11_SDK_VERSION,
                           &device,
                           &featureLevel,
                           &deviceContext);
    if (FAILED(hr)) {
        SDL_LogError(SDL_LOG_CATEGORY_APPLICATION,
                     "D3D11CreateDevice() failed: %x",
                     hr);
        return false;
    }

    hr = device.As(&m_DecodeDevice);
    if (FAILED(hr)) {
        SDL_LogError(SDL_LOG_CATEGORY_APPLICATION,
                     "ID3D11Device::QueryInterface(ID3D11Device1) failed: %x",
                     hr);
        goto Exit;
    }

    hr = deviceContext.As(&m_DecodeDeviceContext);
    if (FAILED(hr)) {
        SDL_LogError(SDL_LOG_CATEGORY_APPLICATION,
                     "ID3D11DeviceContext::QueryInterface(ID3D11DeviceContext1) failed: %x",
                     hr);
        goto Exit;
    }

    // Create our decode->render fence
    m_D2RFenceValue = 1;
    if (!createSharedFencePair(0, m_DecodeDevice.Get(), m_RenderDevice.Get(), m_DecodeD2RFence, m_RenderD2RFence)) {
        goto Exit;
    }

    // Create our render->decode fence
    m_R2DFenceValue = 1;
    if (!createSharedFencePair(0, m_DecodeDevice.Get(), m_RenderDevice.Get(), m_DecodeR2DFence, m_RenderR2DFence)) {
        goto Exit;
    }

    success = true;
Exit:
    if (!success) {
        m_DecodeD2RFence.Reset();
        m_RenderD2RFence.Reset();
        m_DecodeR2DFence.Reset();
        m_RenderR2DFence.Reset();
        m_DecodeDevice.Reset();
    }

    return success;
}

static void highResolutionSleepUs(uint64_t sleepUs)
{
    static thread_local HANDLE waitableTimer =
        CreateWaitableTimerExW(nullptr, nullptr, CREATE_WAITABLE_TIMER_HIGH_RESOLUTION, TIMER_ALL_ACCESS);
    if (waitableTimer == nullptr) {
        return;
    }

    LARGE_INTEGER dueTime;
    dueTime.QuadPart = -((LONGLONG)sleepUs * 10);
    if (SetWaitableTimer(waitableTimer, &dueTime, 0, nullptr, nullptr, FALSE)) {
        WaitForSingleObject(waitableTimer, INFINITE);
    }
}

bool D3D11VARenderer::queryTearingSupport(HRESULT* result)
{
    BOOL allowTearing = FALSE;
    HRESULT hr = m_Factory->CheckFeatureSupport(DXGI_FEATURE_PRESENT_ALLOW_TEARING,
                                                &allowTearing,
                                                sizeof(allowTearing));

    if (result != nullptr) {
        *result = hr;
    }

    if (FAILED(hr)) {
        SDL_LogError(SDL_LOG_CATEGORY_APPLICATION,
                     "IDXGIFactory::CheckFeatureSupport(DXGI_FEATURE_PRESENT_ALLOW_TEARING) failed: %x",
                     hr);
        return false;
    }

    return allowTearing == TRUE;
}

void D3D11VARenderer::resolvePresentationMode(SDL_Window* window, DXGI_SWAP_CHAIN_DESC1* swapChainDesc)
{
    const bool disableVrr = qEnvironmentVariableIntValue("MOONLIGHT_DISABLE_VRR") != 0;
    const bool forceVrr = qEnvironmentVariableIntValue("MOONLIGHT_FORCE_VRR") != 0 && !disableVrr;
    const Uint32 windowFlags = SDL_GetWindowFlags(window);
    const bool fullscreenExclusive = (windowFlags & SDL_WINDOW_FULLSCREEN_DESKTOP) == SDL_WINDOW_FULLSCREEN;
    const int displayFps = StreamUtils::getDisplayRefreshRate(window);
    const bool withinDisplayHz = m_DecoderParams.frameRate <= displayFps;
    HRESULT tearingHr = S_OK;

    m_TearingSupport = queryTearingSupport(&tearingHr);
    m_AllowTearing = false;

    const char* fallbackReason = nullptr;

    if (disableVrr && m_DecoderParams.enableVsync) {
        m_PresentationMode = PresentationMode::FixedVsync;
        fallbackReason = "MOONLIGHT_DISABLE_VRR is set";
    }
    else if (!m_TearingSupport) {
        m_PresentationMode = m_DecoderParams.enableVsync ?
            PresentationMode::FixedVsync :
            PresentationMode::Immediate;
        fallbackReason = SUCCEEDED(tearingHr) ?
            "DXGI_FEATURE_PRESENT_ALLOW_TEARING is not supported" :
            "DXGI tearing support query failed";
    }
    else if (forceVrr) {
        m_PresentationMode = PresentationMode::VrrCadence;
        swapChainDesc->Flags |= DXGI_SWAP_CHAIN_FLAG_ALLOW_TEARING;
        m_AllowTearing = true;
        fallbackReason = "MOONLIGHT_FORCE_VRR is set";
    }
    else if (!m_DecoderParams.enableVsync) {
        m_PresentationMode = PresentationMode::Immediate;
        swapChainDesc->Flags |= DXGI_SWAP_CHAIN_FLAG_ALLOW_TEARING;
        m_AllowTearing = true;
        // Session force-disables V-sync when the stream FPS exceeds the
        // display's current refresh rate, so surface that possibility - a
        // panel quietly dropping to 60Hz (power saving) makes VRR appear
        // "broken" with no other visible signal.
        fallbackReason = withinDisplayHz ?
            "V-sync is disabled" :
            "V-sync auto-disabled: stream FPS exceeds display refresh rate";
    }
    else if (fullscreenExclusive && !forceVrr) {
        m_PresentationMode = PresentationMode::FixedVsync;
        fallbackReason = "exclusive fullscreen does not use windowed DXGI tearing";
    }
    else if (!withinDisplayHz && !forceVrr) {
        m_PresentationMode = PresentationMode::FixedVsync;
        fallbackReason = "stream FPS exceeds display refresh rate";
    }
    else {
        m_PresentationMode = PresentationMode::VrrCadence;
        swapChainDesc->Flags |= DXGI_SWAP_CHAIN_FLAG_ALLOW_TEARING;
        m_AllowTearing = true;
    }

    m_PresentationModeFallbackReason = fallbackReason;

    logPresentationMode(window, swapChainDesc, m_OutputIndex, fallbackReason);
}

void D3D11VARenderer::logPresentationMode(SDL_Window* window,
                                          const DXGI_SWAP_CHAIN_DESC1* swapChainDesc,
                                          int outputIndex,
                                          const char* fallbackReason)
{
    const bool presentationLog = qEnvironmentVariableIntValue("MOONLIGHT_PRESENTATION_MODE_LOG") != 0;
    const Uint32 windowFlags = SDL_GetWindowFlags(window);
    const char* windowMode;

    if ((windowFlags & SDL_WINDOW_FULLSCREEN_DESKTOP) == SDL_WINDOW_FULLSCREEN) {
        windowMode = "exclusive-fullscreen";
    }
    else if (windowFlags & SDL_WINDOW_FULLSCREEN_DESKTOP) {
        windowMode = "borderless";
    }
    else {
        windowMode = "windowed";
    }

    SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION,
                "D3D11VA presentation: mode=%s tearing=%d tearing-flag=%d display=%d Hz stream=%d FPS window=%s swapchain-format=%d buffers=%u alpha=%u swapchain-flags=0x%x present-flags=0x%x",
                getPresentationModeName(m_PresentationMode),
                m_TearingSupport,
                m_AllowTearing,
                StreamUtils::getDisplayRefreshRate(window),
                m_DecoderParams.frameRate,
                windowMode,
                swapChainDesc->Format,
                swapChainDesc->BufferCount,
                swapChainDesc->AlphaMode,
                m_AllowTearing ? DXGI_SWAP_CHAIN_FLAG_ALLOW_TEARING : 0,
                m_AllowTearing ? DXGI_PRESENT_ALLOW_TEARING : 0);

    if (fallbackReason != nullptr &&
            (m_PresentationMode != PresentationMode::VrrCadence || presentationLog)) {
        SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION,
                    "D3D11VA presentation note: %s",
                    fallbackReason);
    }

    if (!presentationLog) {
        return;
    }

    ComPtr<IDXGIAdapter1> adapter;
    HRESULT hr = m_Factory->EnumAdapters1(m_AdapterIndex, &adapter);
    if (SUCCEEDED(hr)) {
        DXGI_ADAPTER_DESC1 adapterDesc;
        if (SUCCEEDED(adapter->GetDesc1(&adapterDesc))) {
            SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION,
                        "D3D11VA presentation adapter: index=%d name=%S vendor=%x device=%x",
                        m_AdapterIndex,
                        adapterDesc.Description,
                        adapterDesc.VendorId,
                        adapterDesc.DeviceId);
        }

        ComPtr<IDXGIOutput> output;
        if (outputIndex >= 0 && SUCCEEDED(adapter->EnumOutputs(outputIndex, &output))) {
            DXGI_OUTPUT_DESC outputDesc;
            if (SUCCEEDED(output->GetDesc(&outputDesc))) {
                SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION,
                            "D3D11VA presentation output: index=%d name=%S desktop=(%ld,%ld)-(%ld,%ld)",
                            outputIndex,
                            outputDesc.DeviceName,
                            outputDesc.DesktopCoordinates.left,
                            outputDesc.DesktopCoordinates.top,
                            outputDesc.DesktopCoordinates.right,
                            outputDesc.DesktopCoordinates.bottom);
            }
        }
    }
}

void D3D11VARenderer::refreshOutput()
{
    m_Output.Reset();
    m_ActiveScanLines = 0;
    m_ScanoutPeriodUs = 0;

    if (m_KmtAdapterValid) {
        D3DKMT_CLOSEADAPTER closeAdapter = {};
        closeAdapter.hAdapter = m_KmtAdapter;
        D3DKMTCloseAdapter(&closeAdapter);
        m_KmtAdapterValid = false;
    }

    if (m_OutputIndex < 0) {
        return;
    }

    ComPtr<IDXGIAdapter1> adapter;
    if (FAILED(m_Factory->EnumAdapters1(m_AdapterIndex, &adapter))) {
        return;
    }

    if (FAILED(adapter->EnumOutputs(m_OutputIndex, &m_Output))) {
        return;
    }

    // Open the matching GDI/KMT adapter handle so we can query the real scan
    // position via D3DKMTGetScanLine() - there is no equivalent on IDXGIOutput
    // itself (IDXGIOutput::GetRasterStatus doesn't exist; that's D3D9-only).
    DXGI_OUTPUT_DESC outputDesc;
    if (FAILED(m_Output->GetDesc(&outputDesc))) {
        return;
    }

    D3DKMT_OPENADAPTERFROMGDIDISPLAYNAME openAdapter = {};
    wcsncpy_s(openAdapter.DeviceName, outputDesc.DeviceName, _TRUNCATE);
    if (D3DKMTOpenAdapterFromGdiDisplayName(&openAdapter) == 0 /* STATUS_SUCCESS */) {
        m_KmtAdapter = openAdapter.hAdapter;
        m_KmtVidPnSourceId = openAdapter.VidPnSourceId;
        m_KmtAdapterValid = true;
    }

    // Cache the scanout geometry/timing that waitForVBlankBeforeTearingPresent()
    // needs to estimate the beam's time-to-blank from a raw scanline number.
    // Scanlines follow the panel's native orientation, not the rotated desktop.
    if (outputDesc.Rotation == DXGI_MODE_ROTATION_ROTATE90 ||
            outputDesc.Rotation == DXGI_MODE_ROTATION_ROTATE270) {
        m_ActiveScanLines = (uint32_t)qMax(0L, outputDesc.DesktopCoordinates.right -
                                               outputDesc.DesktopCoordinates.left);
    }
    else {
        m_ActiveScanLines = (uint32_t)qMax(0L, outputDesc.DesktopCoordinates.bottom -
                                               outputDesc.DesktopCoordinates.top);
    }

    // Under VRR the line clock stays at the max refresh rate's speed and only
    // the blanking gap stretches, so the current mode's refresh rate gives the
    // scanout duration regardless of the instantaneous refresh interval.
    int displayFps = StreamUtils::getDisplayRefreshRate(m_DecoderParams.window);
    m_ScanoutPeriodUs = displayFps > 0 ? 1000000ULL / displayFps : 0;
}

void D3D11VARenderer::waitForVBlankBeforeTearingPresent(uint64_t alignBudgetUs)
{
    // VRR only changes how long the panel is willing to extend its blanking
    // gap while waiting for us - it does not make a DXGI_PRESENT_ALLOW_TEARING
    // present synchronize to scan position on its own. A tearing present that
    // lands while the panel is actively mid-scan still tears, no matter how
    // well the cadence clock has paced the interval between presents.
    //
    // Public DXGI has no scanline/vblank query at all (IDXGIOutput::GetRasterStatus
    // is D3D9-only and doesn't exist here; IDXGIOutput::WaitForVBlank() is an
    // unbounded blocking call with unverified behavior under VRR's extended
    // blanking - risked adding up to a full extra refresh interval of latency
    // if it doesn't return immediately when already inside the blanking gap).
    //
    // D3DKMTGetScanLine() is the same low-level scan-position query used by
    // Special K's "Latent Sync" and proposed for RetroArch's beam-racing work
    // (with WinUAE as a working tear-free-VSYNC-OFF-under-VRR precedent) - and
    // unlike WaitForVBlank, it's a plain non-blocking query, so we can poll it
    // in a tightly bounded loop instead of risking an open-ended block.
    if (!m_KmtAdapterValid || qEnvironmentVariableIntValue("MOONLIGHT_DISABLE_SCANLINE_ALIGN")) {
        return;
    }

    // The wait bound is the pacer's per-present budget, sized from the
    // measured content cadence so waiting can never starve the next frame
    // (see Pacer::cadenceThread). This replaces the old fixed 3ms spin,
    // whose cap was exactly what made the free-running-raster state sticky:
    // once torn flips knock the driver out of VRR flip-following, presents
    // land at random scan phase and a 3ms window only catches the blank
    // ~40% of the time, so the tearing regime self-sustains even at content
    // rates with milliseconds of idle headroom per frame. One scanout cycle
    // plus slack is always enough waiting: even a free-running raster
    // passes through its blanking gap within a cycle.
    //
    // MOONLIGHT_VRR_SCANLINE_SLEEP=1 forces the widest bound on every
    // present regardless of the pacer's budget, for A/B testing.
    if (qEnvironmentVariableIntValue("MOONLIGHT_VRR_SCANLINE_SLEEP") != 0 && m_ScanoutPeriodUs != 0) {
        alignBudgetUs = m_ScanoutPeriodUs + 2000;
    }

    uint64_t maxWaitUs = m_ScanoutPeriodUs != 0 ?
        qMin(alignBudgetUs, m_ScanoutPeriodUs + 2000) :
        qMin(alignBudgetUs, (uint64_t)3000);

    uint64_t startUs = LiGetMicroseconds();
    D3DKMT_GETSCANLINE getScanLine = {};
    getScanLine.hAdapter = m_KmtAdapter;
    getScanLine.VidPnSourceId = m_KmtVidPnSourceId;

    // Query at least once even with a zero budget: a rush present that
    // happens to arrive inside the blanking gap still flips clean for the
    // cost of one syscall.
    const bool classicPresent =
        qEnvironmentVariableIntValue("MOONLIGHT_VRR_CLASSIC_PRESENT") != 0;
    bool reachedBlank = false;
    for (;;) {
        if (D3DKMTGetScanLine(&getScanLine) != 0 /* STATUS_SUCCESS */ || getScanLine.InVerticalBlank) {
            reachedBlank = true;
            break;
        }

        uint64_t elapsedUs = LiGetMicroseconds() - startUs;
        if (elapsedUs >= maxWaitUs) {
            break;
        }

        if (m_ActiveScanLines != 0 && m_ScanoutPeriodUs != 0 &&
                getScanLine.ScanLine < m_ActiveScanLines) {
            uint64_t remainingUs = (uint64_t)(m_ActiveScanLines - getScanLine.ScanLine) *
                    m_ScanoutPeriodUs / (m_ActiveScanLines + m_ActiveScanLines / 20);

            // Give up immediately when the blanking gap is out of reach
            // within the remaining budget: waiting toward a guaranteed
            // mid-scan present doesn't move the tear, it only adds latency
            // and flip-phase noise. This is what turns a small budget into
            // a phase SNAP - presents already near the blank align to it,
            // everything else goes out untouched at pure content cadence,
            // which above the panel's flip ceiling keeps the beat's tear
            // line herded toward the screen edge instead of crawling
            // mid-frame. The line-time estimate deliberately errs low
            // (total lines padded ~5% over active for VBI overhead), so
            // borderline frames still wait and try. Classic mode keeps the
            // historical spin-out-the-full-bound behavior for faithful A/B.
            if (!classicPresent && remainingUs > maxWaitUs - elapsedUs) {
                break;
            }

            // While the beam is still far from the blanking gap, sleep off
            // most of the remaining scanout instead of hard-spinning a
            // TIME_CRITICAL thread for up to a whole refresh period; only
            // the final approach spins. Clamped to the remaining budget so
            // the beam estimate can't carry the wait past a tighter bound.
            remainingUs = qMin(remainingUs, maxWaitUs - elapsedUs);
            if (remainingUs > 1500) {
                highResolutionSleepUs(remainingUs - 1000);
            }
        }
    }

    uint64_t waitedUs = LiGetMicroseconds() - startUs;
    m_LastPresentAlignmentWaitUs += waitedUs;

    // Outcome telemetry for the residual-tear hunt: every give-up below is a
    // present that went out mid-scan, i.e. one visible tear, while a skip is
    // a present that never meaningfully tried (a rush with no cadence slack).
    // This is the ground truth to A/B the budget policy against.
    if (reachedBlank) {
        m_AlignHits++;
    }
    else if (maxWaitUs < 500) {
        m_AlignSkips++;
    }
    else {
        m_AlignGiveUps++;
    }
    m_AlignWaitTotalUs += waitedUs;
    m_AlignBudgetTotalUs += maxWaitUs;

    uint64_t nowUs = startUs + waitedUs;
    if (m_AlignStatsStartUs == 0) {
        m_AlignStatsStartUs = nowUs;
    }
    else if (nowUs - m_AlignStatsStartUs >= 10000000) {
        uint32_t attempted = m_AlignHits + m_AlignGiveUps;
        uint32_t total = attempted + m_AlignSkips;
        SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION,
                    "VRR scanline align: %u in-blank, %u mid-scan tears (%.1f%%), avg wait %.2f ms, avg budget %.2f ms, %u skipped-late",
                    m_AlignHits,
                    m_AlignGiveUps,
                    attempted != 0 ? m_AlignGiveUps * 100.0 / attempted : 0.0,
                    attempted != 0 ? (m_AlignWaitTotalUs / 1000.0) / attempted : 0.0,
                    total != 0 ? (m_AlignBudgetTotalUs / 1000.0) / total : 0.0,
                    m_AlignSkips);
        m_AlignHits = 0;
        m_AlignGiveUps = 0;
        m_AlignSkips = 0;
        m_AlignWaitTotalUs = 0;
        m_AlignBudgetTotalUs = 0;
        m_AlignStatsStartUs = nowUs;
    }
}

bool D3D11VARenderer::signalAndUnlockForPresent()
{
    // Present() doesn't flip at the instant we call it; the flip executes only
    // once all queued GPU work for the back buffer has completed. Measured on
    // the Ally X: ~2.4ms average Present-to-screen lag (PresentMon
    // MsUntilDisplayed), which is wider than the entire blanking window when
    // the stream FPS runs near the display's max refresh - so aligning the
    // Present() call to the blank is aligning the wrong instant, and the flip
    // itself still tears. Fence the end of our rendering and wait for the GPU
    // BEFORE the target-hold and scanline alignment, so Present() becomes the
    // true flip instant.
    //
    // Critically, the wait happens on a fence event with the context lock
    // RELEASED: decode shares this device on AMD APUs, so a blocking wait
    // under the lock stalls the decoder for milliseconds every frame (the
    // mistake that sank the first version of this fix).
    if (m_PresentReadyFence == nullptr && !m_PresentReadyFenceFailed) {
        HRESULT hr = m_RenderDevice->CreateFence(0, D3D11_FENCE_FLAG_NONE,
                                                 IID_PPV_ARGS(&m_PresentReadyFence));
        if (SUCCEEDED(hr)) {
            m_PresentReadyFenceEvent = CreateEventW(nullptr, FALSE, FALSE, nullptr);
        }

        if (FAILED(hr) || m_PresentReadyFenceEvent == nullptr) {
            SDL_LogWarn(SDL_LOG_CATEGORY_APPLICATION,
                        "No present-ready fence available (%x); presents will lag GPU completion",
                        hr);
            m_PresentReadyFence.Reset();
            m_PresentReadyFenceFailed = true;
        }
    }

    if (m_PresentReadyFence == nullptr) {
        return false;
    }

    uint64_t fenceValue = ++m_PresentReadyFenceValue;
    if (FAILED(m_RenderDeviceContext->Signal(m_PresentReadyFence.Get(), fenceValue))) {
        m_PresentReadyFenceFailed = true;
        m_PresentReadyFence.Reset();
        return false;
    }

    m_RenderDeviceContext->Flush();

    if (m_DecodeDevice == m_RenderDevice) {
        unlockContext(this);
    }

    if (SUCCEEDED(m_PresentReadyFence->SetEventOnCompletion(fenceValue, m_PresentReadyFenceEvent))) {
        // Bounded so a wedged GPU can't hang the pacing thread forever
        WaitForSingleObject(m_PresentReadyFenceEvent, 50);
    }

    return true;
}

uint64_t D3D11VARenderer::holdUntilPresentTarget()
{
    // Hold the flip until the cadence pacer's intended instant. Without this,
    // a present is released as soon as rendering finishes - up to the whole
    // render-lead early - which re-scatters the present phase the pacer just
    // computed and can space flips tighter than the panel's max refresh
    // (observed as 5.5-7.4ms minimum intervals against an 8.33ms floor).
    //
    // Returns how far past the target we already were on entry (0 if we
    // held), so the caller knows the schedule is running behind.
    uint64_t targetUs = m_PresentTargetUs;
    m_PresentTargetUs = 0;

    if (targetUs == 0) {
        return 0;
    }

    uint64_t startUs = LiGetMicroseconds();
    if (startUs > targetUs) {
        return startUs - targetUs;
    }

    uint64_t nowUs = startUs;
    while (nowUs < targetUs) {
        if (targetUs - nowUs > 1500) {
            highResolutionSleepUs(targetUs - nowUs - 1000);
        }
        nowUs = LiGetMicroseconds();
    }

    m_LastPresentAlignmentWaitUs += nowUs - startUs;
    return 0;
}

bool D3D11VARenderer::createDeviceByAdapterIndex(int adapterIndex, bool* adapterNotFound)
{
    const D3D_FEATURE_LEVEL supportedFeatureLevels[] = { D3D_FEATURE_LEVEL_11_1, D3D_FEATURE_LEVEL_11_0 };
    bool success = false;
    ComPtr<IDXGIAdapter1> adapter;
    DXGI_ADAPTER_DESC1 adapterDesc;
    D3D_FEATURE_LEVEL featureLevel;
    HRESULT hr;
    ComPtr<ID3D11Device> device;
    ComPtr<ID3D11DeviceContext> deviceContext;

    SDL_assert(!m_RenderDevice);
    SDL_assert(!m_RenderDeviceContext);
    SDL_assert(!m_DecodeDevice);
    SDL_assert(!m_DecodeDeviceContext);

    hr = m_Factory->EnumAdapters1(adapterIndex, &adapter);
    if (hr == DXGI_ERROR_NOT_FOUND) {
        // Expected at the end of enumeration
        goto Exit;
    }
    else if (FAILED(hr)) {
        SDL_LogError(SDL_LOG_CATEGORY_APPLICATION,
                     "IDXGIFactory::EnumAdapters1() failed: %x",
                     hr);
        goto Exit;
    }

    hr = adapter->GetDesc1(&adapterDesc);
    if (FAILED(hr)) {
        SDL_LogError(SDL_LOG_CATEGORY_APPLICATION,
                     "IDXGIAdapter::GetDesc() failed: %x",
                     hr);
        goto Exit;
    }

    if (adapterDesc.Flags & DXGI_ADAPTER_FLAG_SOFTWARE) {
        // Skip the WARP device. We know it will fail.
        goto Exit;
    }

    SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION,
                "Detected GPU %d: %S (%x:%x)",
                adapterIndex,
                adapterDesc.Description,
                adapterDesc.VendorId,
                adapterDesc.DeviceId);

    hr = D3D11CreateDevice(adapter.Get(),
                           D3D_DRIVER_TYPE_UNKNOWN,
                           nullptr,
                           D3D11_CREATE_DEVICE_VIDEO_SUPPORT
                               | (m_DebugLayer ? D3D11_CREATE_DEVICE_DEBUG : 0),
                           supportedFeatureLevels,
                           ARRAYSIZE(supportedFeatureLevels),
                           D3D11_SDK_VERSION,
                           &device,
                           &featureLevel,
                           &deviceContext);
    if (FAILED(hr)) {
        SDL_LogError(SDL_LOG_CATEGORY_APPLICATION,
                     "D3D11CreateDevice() failed: %x",
                     hr);
        goto Exit;
    }
    else if (adapterDesc.VendorId == 0x8086 && featureLevel <= D3D_FEATURE_LEVEL_11_0 && !qEnvironmentVariableIntValue("D3D11VA_ENABLED")) {
        SDL_LogWarn(SDL_LOG_CATEGORY_APPLICATION,
                    "Avoiding D3D11VA on old pre-FL11.1 Intel GPU. Set D3D11VA_ENABLED=1 to override.");
        goto Exit;
    }
    else if (featureLevel >= D3D_FEATURE_LEVEL_11_0) {
        // Remember that we found a non-software D3D11 devices with support for
        // feature level 11.0 or later (Fermi, Terascale 2, or Ivy Bridge and later)
        m_DevicesWithFL11Support++;
    }

    hr = device.As(&m_RenderDevice);
    if (FAILED(hr)) {
        SDL_LogError(SDL_LOG_CATEGORY_APPLICATION,
                     "ID3D11Device::QueryInterface(ID3D11Device1) failed: %x",
                     hr);
        goto Exit;
    }

    hr = deviceContext.As(&m_RenderDeviceContext);
    if (FAILED(hr)) {
        SDL_LogError(SDL_LOG_CATEGORY_APPLICATION,
                     "ID3D11DeviceContext::QueryInterface(ID3D11DeviceContext1) failed: %x",
                     hr);
        goto Exit;
    }

    // Check which fence types are supported by this GPU
    {
        m_FenceType = SupportedFenceType::None;

        ComPtr<IDXGIAdapter4> adapter4;
        if (SUCCEEDED(adapter.As(&adapter4))) {
            DXGI_ADAPTER_DESC3 desc3;
            if (SUCCEEDED(adapter4->GetDesc3(&desc3))) {
                if (desc3.Flags & DXGI_ADAPTER_FLAG3_SUPPORT_MONITORED_FENCES) {
                    // Monitored fences must be used when they are supported
                    m_FenceType = SupportedFenceType::Monitored;
                }
                else if (desc3.Flags & DXGI_ADAPTER_FLAG3_SUPPORT_NON_MONITORED_FENCES) {
                    // Non-monitored fences must only be used when monitored fences are unsupported
                    m_FenceType = SupportedFenceType::NonMonitored;
                }
            }
        }
    }

    bool separateDevices;
    if (Utils::getEnvironmentVariableOverride("D3D11VA_FORCE_SEPARATE_DEVICES", &separateDevices)) {
        SDL_LogWarn(SDL_LOG_CATEGORY_APPLICATION,
                    "Using D3D11VA_FORCE_SEPARATE_DEVICES to override default logic");
    }
    else {
        D3D11_FEATURE_DATA_D3D11_OPTIONS d3d11Options;

        // Check if cross-device sharing works for YUV textures and fences are supported
        hr = m_RenderDevice->CheckFeatureSupport(D3D11_FEATURE_D3D11_OPTIONS, &d3d11Options, sizeof(d3d11Options));
        separateDevices = SUCCEEDED(hr) && d3d11Options.ExtendedResourceSharing && m_FenceType != SupportedFenceType::None;

        if (separateDevices) {
            // The Radon HD 5570 GPU drivers deadlock when decoding into shared texture arrays, so let's
            // limit usage of separate devices to FL 11.1+ GPUs to try to exclude old GPU drivers. We'll
            // exempt Intel GPUs because those have been confirmed to work properly (and the extra fence
            // that this device separation uses acts as a workaround for a bug in their old drivers where
            // they don't properly synchronize between decoder output usage and SRV usage).
            if (featureLevel < D3D_FEATURE_LEVEL_11_1 && adapterDesc.VendorId != 0x8086) {
                SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION,
                            "Avoiding texture sharing for old pre-FL11.1 GPU");
                separateDevices = false;
            }
            else if (adapterDesc.VendorId == 0x1ED5 || // Moore Threads (texture is all zero/green)
                     adapterDesc.VendorId == 0x4D4F4351) { // Qualcomm (decoding is unstable/slow on QC710)
                SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION,
                            "Avoiding texture sharing on known broken GPU vendor");
                separateDevices = false;
            }
            else if (adapterDesc.VendorId == 0x1002) { // AMD
                SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION,
                            "Avoiding separate D3D11VA decode/render devices on AMD");
                separateDevices = false;
            }
        }
    }

    // If we're going to use separate devices for decoding and rendering, create the decoding device
    if (!separateDevices || !setupSharedDevice(adapter.Get())) {
        m_DecodeDevice = m_RenderDevice;
        m_DecodeDeviceContext = m_RenderDeviceContext;
        separateDevices = false;
    }

    if (Utils::getEnvironmentVariableOverride("D3D11VA_FORCE_BIND", &m_BindDecoderOutputTextures)) {
        SDL_LogWarn(SDL_LOG_CATEGORY_APPLICATION,
                    "Using D3D11VA_FORCE_BIND to override default bind/copy logic");
    }
    else {
        // Skip copying to our own internal texture on Intel GPUs due to
        // significant performance impact of the extra copy. See:
        // https://github.com/moonlight-stream/moonlight-qt/issues/1304
        //
        // Binding shared decoder texture arrays directly can fail on some
        // AMD Main10 paths even when separate decode/render devices are
        // available. Keep the direct-bind optimization limited to Intel by
        // default and use the safer copy path elsewhere.
        m_BindDecoderOutputTextures = adapterDesc.VendorId == 0x8086;
    }

    SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION,
                "Decoder texture access: %s (fence: %s)",
                m_BindDecoderOutputTextures ? "bind" : "copy",
                 m_FenceType == SupportedFenceType::Monitored ? "monitored" :
                    (m_FenceType == SupportedFenceType::NonMonitored ? "non-monitored" : "unsupported"));

    SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION,
                "Using %s device for decoding and rendering",
                separateDevices ? "separate" : "shared");

    if (!checkDecoderSupport(adapter.Get())) {
        goto Exit;
    }
    else {
        // Remember that we found a device with support for decoding this codec
        m_DevicesWithCodecSupport++;
    }

    if (adapterDesc.VendorId == 0x1002 &&
            (m_DecoderParams.videoFormat & VIDEO_FORMAT_MASK_10BIT) &&
            qEnvironmentVariableIntValue("D3D11VA_AMD_10BIT_DISABLED")) {
        SDL_LogWarn(SDL_LOG_CATEGORY_APPLICATION,
                    "Avoiding D3D11VA on AMD 10-bit video because D3D11VA_AMD_10BIT_DISABLED is set.");
        goto Exit;
    }

    success = true;

Exit:
    if (adapterNotFound != nullptr) {
        *adapterNotFound = !adapter;
    }
    if (!success) {
        m_RenderDeviceContext.Reset();
        m_RenderDevice.Reset();
        m_DecodeDeviceContext.Reset();
        m_DecodeDevice.Reset();
    }
    return success;
}

bool D3D11VARenderer::initialize(PDECODER_PARAMETERS params)
{
    int outputIndex;
    HRESULT hr;

    m_DecoderParams = *params;

    if (qgetenv("D3D11VA_ENABLED") == "0") {
        SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION,
                    "D3D11VA is disabled by environment variable");
        return false;
    }

    if (Utils::getEnvironmentVariableOverride("D3D11VA_DEBUG_LAYER", &m_DebugLayer)) {
        SDL_LogWarn(SDL_LOG_CATEGORY_APPLICATION,
                    "Using D3D11VA_DEBUG_LAYER to override default debug layer behavior");
    }
    else {
#ifdef QT_DEBUG
        m_DebugLayer = true;
#else
        m_DebugLayer = false;
#endif
    }

    // Check if Graphics Tools are installed
    if (m_DebugLayer) {
        HMODULE dxgiDebug = LoadLibraryExW(L"DXGIDebug.dll", 0, LOAD_LIBRARY_SEARCH_SYSTEM32);
        if (dxgiDebug) {
            FreeLibrary(dxgiDebug);
        }
        else {
            SDL_LogError(SDL_LOG_CATEGORY_APPLICATION,
                         "DXGI/D3D11 debug layer unavailable. Enable 'Graphics Tools' optional feature!");
            m_DebugLayer = false;
        }
    }

    if (!SDL_DXGIGetOutputInfo(SDL_GetWindowDisplayIndex(params->window),
                               &m_AdapterIndex, &outputIndex)) {
        SDL_LogError(SDL_LOG_CATEGORY_APPLICATION,
                     "SDL_DXGIGetOutputInfo() failed: %s",
                     SDL_GetError());
        return false;
    }
    m_OutputIndex = outputIndex;

    hr = CreateDXGIFactory2(
        m_DebugLayer ? DXGI_CREATE_FACTORY_DEBUG : 0,
        __uuidof(IDXGIFactory5),
        (void**)&m_Factory);
    if (FAILED(hr)) {
        SDL_LogError(SDL_LOG_CATEGORY_APPLICATION,
                     "CreateDXGIFactory() failed: %x",
                     hr);
        return false;
    }

    refreshOutput();

    // First try the adapter corresponding to the display where our window resides.
    // This will let us avoid a copy if the display GPU has the required decoder.
    if (!createDeviceByAdapterIndex(m_AdapterIndex)) {
        // If that didn't work, we'll try all GPUs in order until we find one
        // or run out of GPUs (DXGI_ERROR_NOT_FOUND from EnumAdapters())
        bool adapterNotFound = false;
        for (int i = 0; !adapterNotFound; i++) {
            if (i == m_AdapterIndex) {
                // Don't try the same GPU again
                continue;
            }

            if (createDeviceByAdapterIndex(i, &adapterNotFound)) {
                // This GPU worked! Continue initialization.
                break;
            }
        }

        if (adapterNotFound) {
            SDL_assert(!m_RenderDevice);
            SDL_assert(!m_RenderDeviceContext);
            return false;
        }
    }

    DXGI_SWAP_CHAIN_DESC1 swapChainDesc = {};
    swapChainDesc.Stereo = FALSE;
    swapChainDesc.SampleDesc.Count = 1;
    swapChainDesc.SampleDesc.Quality = 0;
    swapChainDesc.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
    swapChainDesc.Scaling = DXGI_SCALING_STRETCH;
    swapChainDesc.SwapEffect = DXGI_SWAP_EFFECT_FLIP_DISCARD;
    swapChainDesc.AlphaMode = DXGI_ALPHA_MODE_IGNORE;
    swapChainDesc.Flags = 0;

    // 3 front buffers (default GetMaximumFrameLatency() count)
    // + 1 back buffer
    // + 1 extra for DWM to hold on to for DirectFlip
    //
    // Even though we allocate 3 front buffers for pre-rendered frames,
    // they won't actually increase presentation latency because we
    // always use SyncInterval 0 which replaces the last one.
    //
    // IDXGIDevice1 has a SetMaximumFrameLatency() function, but counter-
    // intuitively we must avoid it to reduce latency. If we set our max
    // frame latency to 1 on thedevice, our SyncInterval 0 Present() calls
    // will block on DWM (acting like SyncInterval 1) rather than doing
    // the non-blocking present we expect. This also defeats VRR: DWM ends up
    // throttling every present to its own cadence regardless of the tearing
    // flag, so the display never actually leaves fixed-rate refresh.
    //
    // NB: 3 total buffers seems sufficient on NVIDIA hardware but
    // causes performance issues (buffer starvation) on AMD GPUs.
    swapChainDesc.BufferCount = 3 + 1 + 1;

    // Use the current window size as the swapchain size
    SDL_GetWindowSize(params->window, (int*)&swapChainDesc.Width, (int*)&swapChainDesc.Height);

    m_DisplayWidth = swapChainDesc.Width;
    m_DisplayHeight = swapChainDesc.Height;

    if (params->videoFormat & VIDEO_FORMAT_MASK_10BIT) {
        swapChainDesc.Format = DXGI_FORMAT_R10G10B10A2_UNORM;
    }
    else {
        swapChainDesc.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
    }

    resolvePresentationMode(params->window, &swapChainDesc);

    // DXVA2 may let us take over for FSE V-sync off cases. However, if we don't have DXGI_FEATURE_PRESENT_ALLOW_TEARING
    // then we should not attempt to do this unless there's no other option (HDR, DXVA2 failed in pass 1, etc).
    if (m_PresentationMode == PresentationMode::Immediate && !m_AllowTearing &&
            m_DecoderSelectionPass == 0 && !(params->videoFormat & VIDEO_FORMAT_MASK_10BIT) &&
            (SDL_GetWindowFlags(params->window) & SDL_WINDOW_FULLSCREEN_DESKTOP) == SDL_WINDOW_FULLSCREEN) {
        SDL_LogWarn(SDL_LOG_CATEGORY_APPLICATION,
                    "Defaulting to DXVA2 for FSE without DXGI_FEATURE_PRESENT_ALLOW_TEARING support");
        return false;
    }

    SDL_SysWMinfo info;
    SDL_VERSION(&info.version);
    SDL_GetWindowWMInfo(params->window, &info);
    SDL_assert(info.subsystem == SDL_SYSWM_WINDOWS);

    // Always use windowed or borderless windowed mode.. SDL does mode-setting for us in
    // full-screen exclusive mode (SDL_WINDOW_FULLSCREEN), so this actually works out okay.
    ComPtr<IDXGISwapChain1> swapChain;
    hr = m_Factory->CreateSwapChainForHwnd(m_RenderDevice.Get(),
                                           info.info.win.window,
                                           &swapChainDesc,
                                           nullptr,
                                           nullptr,
                                           &swapChain);

    if (FAILED(hr)) {
        SDL_LogError(SDL_LOG_CATEGORY_APPLICATION,
                     "IDXGIFactory::CreateSwapChainForHwnd() failed: %x",
                     hr);
        return false;
    }

    hr = swapChain.As(&m_SwapChain);
    if (FAILED(hr)) {
        SDL_LogError(SDL_LOG_CATEGORY_APPLICATION,
                     "IDXGISwapChain::QueryInterface(IDXGISwapChain4) failed: %x",
                     hr);
        return false;
    }

    // Disable Alt+Enter, PrintScreen, and window message snooping. This makes
    // it safe to run the renderer on a separate rendering thread rather than
    // requiring the main (message loop) thread.
    hr = m_Factory->MakeWindowAssociation(info.info.win.window, DXGI_MWA_NO_WINDOW_CHANGES);
    if (FAILED(hr)) {
        SDL_LogError(SDL_LOG_CATEGORY_APPLICATION,
                     "IDXGIFactory::MakeWindowAssociation() failed: %x",
                     hr);
        return false;
    }

    {
        m_HwDeviceContext = av_hwdevice_ctx_alloc(AV_HWDEVICE_TYPE_D3D11VA);
        if (!m_HwDeviceContext) {
            SDL_LogError(SDL_LOG_CATEGORY_APPLICATION,
                        "Failed to allocate D3D11VA device context");
            return false;
        }

        AVHWDeviceContext* deviceContext = (AVHWDeviceContext*)m_HwDeviceContext->data;
        AVD3D11VADeviceContext* d3d11vaDeviceContext = (AVD3D11VADeviceContext*)deviceContext->hwctx;

        // FFmpeg will take ownership of these pointers, so we use CopyTo() to bump the ref count
        m_DecodeDevice.CopyTo(&d3d11vaDeviceContext->device);
        m_DecodeDeviceContext.CopyTo(&d3d11vaDeviceContext->device_context);

        // Set lock functions that we will use to synchronize with FFmpeg's usage of our device context
        d3d11vaDeviceContext->lock = lockContext;
        d3d11vaDeviceContext->unlock = unlockContext;
        d3d11vaDeviceContext->lock_ctx = this;

        int err = av_hwdevice_ctx_init(m_HwDeviceContext);
        if (err < 0) {
            SDL_LogError(SDL_LOG_CATEGORY_APPLICATION,
                         "Failed to initialize D3D11VA device context: %d",
                         err);
            return false;
        }
    }

    if (!setupRenderingResources()) {
        return false;
    }

    return true;
}

bool D3D11VARenderer::prepareDecoderContext(AVCodecContext* context, AVDictionary**)
{
    context->hw_device_ctx = av_buffer_ref(m_HwDeviceContext);

    SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION,
                "Using D3D11VA accelerated renderer");

    return true;
}

bool D3D11VARenderer::prepareDecoderContextInGetFormat(AVCodecContext *context, AVPixelFormat pixelFormat)
{
    // Create a new hardware frames context suitable for decoding our specified format
    av_buffer_unref(&context->hw_frames_ctx);
    int err = avcodec_get_hw_frames_parameters(context, m_HwDeviceContext, pixelFormat, &context->hw_frames_ctx);
    if (err < 0) {
        SDL_LogError(SDL_LOG_CATEGORY_APPLICATION,
                     "Failed to get hwframes context parameters: %d",
                     err);
        return false;
    }

    auto framesContext = (AVHWFramesContext*)context->hw_frames_ctx->data;
    auto d3d11vaFramesContext = (AVD3D11VAFramesContext*)framesContext->hwctx;

    // If we're binding output textures directly, we need to add the SRV bind flag
    if (m_BindDecoderOutputTextures) {
        d3d11vaFramesContext->BindFlags |= D3D11_BIND_SHADER_RESOURCE;
    }

    // If we're using separate decode and render devices, we need to create shared textures
    if (m_DecodeDevice != m_RenderDevice) {
        d3d11vaFramesContext->MiscFlags |= D3D11_RESOURCE_MISC_SHARED | D3D11_RESOURCE_MISC_SHARED_NTHANDLE;
    }

    // Mimic the logic in ff_decode_get_hw_frames_ctx() which adds an extra 3 frames
    if (framesContext->initial_pool_size) {
        framesContext->initial_pool_size += 3;
    }

    err = av_hwframe_ctx_init(context->hw_frames_ctx);
    if (err < 0) {
        av_buffer_unref(&context->hw_frames_ctx);
        SDL_LogError(SDL_LOG_CATEGORY_APPLICATION,
                     "Failed initialize hwframes context: %d",
                     err);
        return false;
    }

    if (!setupFrameRenderingResources(framesContext)) {
        av_buffer_unref(&context->hw_frames_ctx);
        return false;
    }

    return true;
}

void D3D11VARenderer::waitToRender()
{
    if (m_FrameLatencyWaitableObject != nullptr) {
        WaitForSingleObjectEx(m_FrameLatencyWaitableObject, 1000, FALSE);
    }
}

void D3D11VARenderer::renderFrame(AVFrame* frame)
{
    // Acquire the context lock for rendering to prevent concurrent
    // access from inside FFmpeg's decoding code
    if (m_DecodeDevice == m_RenderDevice) {
        lockContext(this);
    }

    // Clear the back buffer
    const float clearColor[4] = {0.0f, 0.0f, 0.0f, 1.0f};
    m_RenderDeviceContext->ClearRenderTargetView(m_RenderTargetView.Get(), clearColor);

    // Bind the back buffer. This needs to be done each time,
    // because the render target view will be unbound by Present().
    m_RenderDeviceContext->OMSetRenderTargets(1, m_RenderTargetView.GetAddressOf(), nullptr);

    // Render our video frame with the aspect-ratio adjusted viewport
    renderVideo(frame);

    // Render overlays on top of the video stream
    for (int i = 0; i < Overlay::OverlayMax; i++) {
        renderOverlay((Overlay::OverlayType)i);
    }

    HRESULT hr;

    if (frame->color_trc != m_LastColorTrc) {
        if (frame->color_trc == AVCOL_TRC_SMPTE2084) {
            // Switch to Rec 2020 PQ (SMPTE ST 2084) colorspace for HDR10 rendering
            hr = m_SwapChain->SetColorSpace1(DXGI_COLOR_SPACE_RGB_FULL_G2084_NONE_P2020);
            if (FAILED(hr)) {
                SDL_LogError(SDL_LOG_CATEGORY_APPLICATION,
                             "IDXGISwapChain::SetColorSpace1(DXGI_COLOR_SPACE_RGB_FULL_G2084_NONE_P2020) failed: %x",
                             hr);
            }
        }
        else {
            // Restore default sRGB colorspace
            hr = m_SwapChain->SetColorSpace1(DXGI_COLOR_SPACE_RGB_FULL_G22_NONE_P709);
            if (FAILED(hr)) {
                SDL_LogError(SDL_LOG_CATEGORY_APPLICATION,
                             "IDXGISwapChain::SetColorSpace1(DXGI_COLOR_SPACE_RGB_FULL_G22_NONE_P709) failed: %x",
                             hr);
            }
        }

        m_LastColorTrc = frame->color_trc;
    }

    UINT flags;
    if (m_AllowTearing) {
        SDL_assert(m_PresentationMode == PresentationMode::Immediate ||
                   m_PresentationMode == PresentationMode::VrrCadence);
        flags = DXGI_PRESENT_ALLOW_TEARING;
    }
    else {
        flags = 0;
    }

    if (m_PresentationMode == PresentationMode::VrrCadence &&
            qEnvironmentVariableIntValue("MOONLIGHT_VRR_CLASSIC_PRESENT") == 0) {
        m_LastPresentAlignmentWaitUs = 0;

        // Three-step phase-corrected present (see each helper for rationale):
        // 1. Fence the GPU and wait for render completion with the context
        //    lock dropped, so Present() below is the true flip instant and
        //    the decoder isn't stalled while we wait.
        // 2. Hold until the pacer's intended present time.
        // 3. Align the last few ms to the panel's blanking gap.
        bool unlocked = signalAndUnlockForPresent();
        if (!unlocked && m_DecodeDevice == m_RenderDevice) {
            // No fence support: still drop the lock for the waits below.
            unlockContext(this);
        }

        // Every present - rush or not - aligns to the blanking gap within
        // the budget the pacer sized for it from the measured cadence. A
        // genuinely late rush at near-max-refresh content gets a ~0 budget
        // and presents immediately (a possible tear still beats compounding
        // lateness into drops), but skipping alignment outright on every
        // rush was what broke the panel's VRR lock: one network stall's
        // burst of deliberately-unaligned presents (454 in one 10s window)
        // dropped the driver back to a fixed free-running raster and locked
        // in minutes of ~60% mid-scan tearing that a 3ms wait could never
        // escape. With headroom, rushes drain a shade slower but aligned,
        // and normal presents may wait up to a full scanout cycle - which
        // is what re-captures the blank once the raster is free-running.
        uint64_t alignBudgetUs = m_PresentAlignBudgetUs;
        m_PresentAlignBudgetUs = 0;
        holdUntilPresentTarget();
        waitForVBlankBeforeTearingPresent(alignBudgetUs);

        // Reacquire the lock for Present(). Any lock delay from an in-flight
        // decode submission lands after alignment and only shifts us deeper
        // into (extended) blanking, which is safe.
        if (m_DecodeDevice == m_RenderDevice) {
            lockContext(this);
        }
    }
    else if (m_PresentationMode == PresentationMode::VrrCadence) {
        // MOONLIGHT_VRR_CLASSIC_PRESENT=1: the original scanline-align-only
        // behavior with its fixed 3ms bound, kept as an instant fallback
        // for A/B testing.
        m_LastPresentAlignmentWaitUs = 0;
        m_PresentTargetUs = 0;
        m_PresentAlignBudgetUs = 0;
        waitForVBlankBeforeTearingPresent(3000);
    }

    // Present according to the selected presentation mode
    m_LastPresentUs = LiGetMicroseconds();
    hr = m_SwapChain->Present(0, flags);

    if (m_DecodeDevice == m_RenderDevice) {
        // Release the context lock
        unlockContext(this);
    }

    if (FAILED(hr)) {
        SDL_LogError(SDL_LOG_CATEGORY_APPLICATION,
                     "IDXGISwapChain::Present() failed: %x",
                     hr);

        // The card may have been removed or crashed. Reset the decoder.
        SDL_Event event;
        event.type = SDL_RENDER_DEVICE_RESET;
        SDL_PushEvent(&event);
        return;
    }
}

void D3D11VARenderer::renderOverlay(Overlay::OverlayType type)
{
    if (!Session::get()->getOverlayManager().isOverlayEnabled(type)) {
        return;
    }

    // If the overlay is being updated, just skip rendering it this frame
    if (!SDL_AtomicTryLock(&m_OverlayLock)) {
        return;
    }

    // Reference these objects so they don't immediately go away if the
    // overlay update thread tries to release them.
    ComPtr<ID3D11Texture2D> overlayTexture = m_OverlayTextures[type];
    ComPtr<ID3D11Buffer> overlayVertexBuffer = m_OverlayVertexBuffers[type];
    ComPtr<ID3D11ShaderResourceView> overlayTextureResourceView = m_OverlayTextureResourceViews[type];
    SDL_AtomicUnlock(&m_OverlayLock);

    if (!overlayTexture) {
        return;
    }

    // If there was a texture, there must also be a vertex buffer and SRV
    SDL_assert(overlayVertexBuffer);
    SDL_assert(overlayTextureResourceView);

    // Bind vertex buffer
    UINT stride = sizeof(VERTEX);
    UINT offset = 0;
    m_RenderDeviceContext->IASetVertexBuffers(0, 1, overlayVertexBuffer.GetAddressOf(), &stride, &offset);

    // Bind pixel shader and resources
    m_RenderDeviceContext->PSSetShader(m_OverlayPixelShader.Get(), nullptr, 0);
    m_RenderDeviceContext->PSSetShaderResources(0, 1, overlayTextureResourceView.GetAddressOf());

    // Draw the overlay with alpha blending
    m_RenderDeviceContext->OMSetBlendState(m_OverlayBlendState.Get(), nullptr, 0xffffffff);
    m_RenderDeviceContext->DrawIndexed(6, 0, 0);
    m_RenderDeviceContext->OMSetBlendState(m_VideoBlendState.Get(), nullptr, 0xffffffff);
}

void D3D11VARenderer::bindVideoVertexBuffer(bool frameChanged, AVFrame* frame)
{
    if (frameChanged || !m_VideoVertexBuffer) {
        // Scale video to the window size while preserving aspect ratio
        SDL_Rect src, dst;
        src.x = src.y = 0;
        src.w = frame->width;
        src.h = frame->height;
        dst.x = dst.y = 0;
        dst.w = m_DisplayWidth;
        dst.h = m_DisplayHeight;
        StreamUtils::scaleSourceToDestinationSurface(&src, &dst);

        // Convert screen space to normalized device coordinates
        SDL_FRect renderRect;
        StreamUtils::screenSpaceToNormalizedDeviceCoords(&dst, &renderRect, m_DisplayWidth, m_DisplayHeight);

        // Don't sample from the alignment padding area
        auto framesContext = (AVHWFramesContext*)frame->hw_frames_ctx->data;
        float uMax = (float)frame->width / framesContext->width;
        float vMax = (float)frame->height / framesContext->height;

        VERTEX verts[] =
        {
            {renderRect.x, renderRect.y, 0, vMax},
            {renderRect.x, renderRect.y+renderRect.h, 0, 0},
            {renderRect.x+renderRect.w, renderRect.y, uMax, vMax},
            {renderRect.x+renderRect.w, renderRect.y+renderRect.h, uMax, 0},
        };

        D3D11_BUFFER_DESC vbDesc = {};
        vbDesc.ByteWidth = sizeof(verts);
        vbDesc.Usage = D3D11_USAGE_IMMUTABLE;
        vbDesc.BindFlags = D3D11_BIND_VERTEX_BUFFER;
        vbDesc.CPUAccessFlags = 0;
        vbDesc.MiscFlags = 0;
        vbDesc.StructureByteStride = sizeof(VERTEX);

        D3D11_SUBRESOURCE_DATA vbData = {};
        vbData.pSysMem = verts;

        HRESULT hr = m_RenderDevice->CreateBuffer(&vbDesc, &vbData, &m_VideoVertexBuffer);
        if (FAILED(hr)) {
            SDL_LogError(SDL_LOG_CATEGORY_APPLICATION,
                         "ID3D11Device::CreateBuffer() failed: %x",
                         hr);
            return;
        }
    }

    // Bind video rendering vertex buffer
    UINT stride = sizeof(VERTEX);
    UINT offset = 0;
    m_RenderDeviceContext->IASetVertexBuffers(0, 1, m_VideoVertexBuffer.GetAddressOf(), &stride, &offset);
}

void D3D11VARenderer::bindColorConversion(bool frameChanged, AVFrame* frame)
{
    bool yuv444 = (m_DecoderParams.videoFormat & VIDEO_FORMAT_MASK_YUV444);
    auto framesContext = (AVHWFramesContext*)frame->hw_frames_ctx->data;

    if (yuv444) {
        // We'll need to use one of the 4:4:4 shaders for this pixel format
        switch (m_TextureFormat)
        {
        case DXGI_FORMAT_AYUV:
            m_RenderDeviceContext->PSSetShader(m_VideoPixelShaders[PixelShaders::GENERIC_AYUV].Get(), nullptr, 0);
            break;
        case DXGI_FORMAT_Y410:
            m_RenderDeviceContext->PSSetShader(m_VideoPixelShaders[PixelShaders::GENERIC_Y410].Get(), nullptr, 0);
            break;
        default:
            SDL_assert(false);
        }
    }
    else {
        // We'll need to use the generic 4:2:0 shader for this colorspace and color range combo
        m_RenderDeviceContext->PSSetShader(m_VideoPixelShaders[PixelShaders::GENERIC_YUV_420].Get(), nullptr, 0);
    }

    // If nothing has changed since last frame, we're done
    if (!frameChanged) {
        return;
    }

    D3D11_BUFFER_DESC constDesc = {};
    constDesc.ByteWidth = sizeof(CSC_CONST_BUF);
    constDesc.Usage = D3D11_USAGE_IMMUTABLE;
    constDesc.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
    constDesc.CPUAccessFlags = 0;
    constDesc.MiscFlags = 0;

    CSC_CONST_BUF constBuf = {};
    std::array<float, 9> cscMatrix;
    std::array<float, 3> yuvOffsets;
    getFramePremultipliedCscConstants(frame, cscMatrix, yuvOffsets);

    std::copy(yuvOffsets.cbegin(), yuvOffsets.cend(), constBuf.offsets);

    // We need to adjust our CSC matrix to be column-major and with float3 vectors
    // padded with a float in between each of them to adhere to HLSL requirements.
    for (int i = 0; i < 3; i++) {
        for (int j = 0; j < 3; j++) {
            constBuf.cscMatrix[i * 4 + j] = cscMatrix[j * 3 + i];
        }
    }

    std::array<float, 2> chromaOffset;
    getFrameChromaCositingOffsets(frame, chromaOffset);
    constBuf.chromaOffset[0] = chromaOffset[0] / framesContext->width;
    constBuf.chromaOffset[1] = chromaOffset[1] / framesContext->height;

    // Limit chroma texcoords to avoid sampling from alignment texels
    constBuf.chromaUVMax[0] = frame->width != framesContext->width ?
                                  ((float)(frame->width - 1) / framesContext->width) : 1.0f;
    constBuf.chromaUVMax[1] = frame->height != (int)framesContext->height ?
                                  ((float)(frame->height - 1) / framesContext->height) : 1.0f;

    D3D11_SUBRESOURCE_DATA constData = {};
    constData.pSysMem = &constBuf;

    ComPtr<ID3D11Buffer> constantBuffer;
    HRESULT hr = m_RenderDevice->CreateBuffer(&constDesc, &constData, &constantBuffer);
    if (SUCCEEDED(hr)) {
        m_RenderDeviceContext->PSSetConstantBuffers(0, 1, constantBuffer.GetAddressOf());
    }
    else {
        SDL_LogError(SDL_LOG_CATEGORY_APPLICATION,
                     "ID3D11Device::CreateBuffer() failed: %x",
                     hr);
        return;
    }
}

void D3D11VARenderer::renderVideo(AVFrame* frame)
{
    // Insert a fence to force the render context to wait for the decode context to finish writing
    if (m_DecodeDevice != m_RenderDevice) {
        SDL_assert(m_DecodeD2RFence);
        SDL_assert(m_RenderD2RFence);

        lockContext(this);
        if (SUCCEEDED(m_DecodeDeviceContext->Signal(m_DecodeD2RFence.Get(), m_D2RFenceValue))) {
            m_RenderDeviceContext->Wait(m_RenderD2RFence.Get(), m_D2RFenceValue++);
        }
        unlockContext(this);
    }

    UINT srvIndex;
    if (m_BindDecoderOutputTextures) {
        // Our indexing logic depends on a direct mapping into m_VideoTextureResourceViews
        // based on the texture index provided by FFmpeg.
        srvIndex = (uintptr_t)frame->data[1];
        SDL_assert(srvIndex < m_VideoTextureResourceViews.size());
        if (srvIndex >= m_VideoTextureResourceViews.size()) {
            SDL_LogError(SDL_LOG_CATEGORY_APPLICATION,
                         "Unexpected texture index: %u",
                         srvIndex);
            return;
        }
    }
    else {
        // Copy this frame into our video texture
        m_RenderDeviceContext->CopySubresourceRegion1(m_VideoTexture.Get(), 0, 0, 0, 0,
                                                      m_RenderSharedTextureArray.Get(),
                                                      (int)(intptr_t)frame->data[1],
                                                      nullptr, D3D11_COPY_DISCARD);

        // SRV 0 is always mapped to the video texture
        srvIndex = 0;
    }

    bool frameChanged = hasFrameFormatChanged(frame);

    // Bind our vertex buffer
    bindVideoVertexBuffer(frameChanged, frame);

    // Bind our CSC shader (and constant buffer, if required)
    bindColorConversion(frameChanged, frame);

    // Bind SRVs for this frame
    ID3D11ShaderResourceView* frameSrvs[] = { m_VideoTextureResourceViews[srvIndex][0].Get(), m_VideoTextureResourceViews[srvIndex][1].Get() };
    m_RenderDeviceContext->PSSetShaderResources(0, 2, frameSrvs);

    // Draw the video
    m_RenderDeviceContext->DrawIndexed(6, 0, 0);

    // Unbind SRVs for this frame
    ID3D11ShaderResourceView* nullSrvs[2] = {};
    m_RenderDeviceContext->PSSetShaderResources(0, 2, nullSrvs);

    // Insert a fence to force the decode context to wait for the render context to finish reading
    if (m_DecodeDevice != m_RenderDevice) {
        SDL_assert(m_DecodeR2DFence);
        SDL_assert(m_RenderR2DFence);

        // Because Pacer keeps a reference to the current frame until the next frame is rendered,
        // we insert a wait for the previous frame's fence value rather than the current one.
        // This means the fence should generally not cause a pipeline bubble for the decoder
        // unless rendering is taking much longer than expected.
        if (SUCCEEDED(m_RenderDeviceContext->Signal(m_RenderR2DFence.Get(), m_R2DFenceValue))) {
            lockContext(this);
            SDL_assert(m_R2DFenceValue > 0);
            m_DecodeDeviceContext->Wait(m_DecodeR2DFence.Get(), m_R2DFenceValue - 1);
            unlockContext(this);
            m_R2DFenceValue++;
        }
    }
}

// This function must NOT use any DXGI or ID3D11DeviceContext methods
// since it can be called on an arbitrary thread!
void D3D11VARenderer::notifyOverlayUpdated(Overlay::OverlayType type)
{
    HRESULT hr;

    SDL_Surface* newSurface = Session::get()->getOverlayManager().getUpdatedOverlaySurface(type);
    bool overlayEnabled = Session::get()->getOverlayManager().isOverlayEnabled(type);
    if (newSurface == nullptr && overlayEnabled) {
        // The overlay is enabled and there is no new surface. Leave the old texture alone.
        return;
    }

    SDL_AtomicLock(&m_OverlayLock);
    ComPtr<ID3D11Texture2D> oldTexture = std::move(m_OverlayTextures[type]);
    ComPtr<ID3D11Buffer> oldVertexBuffer = std::move(m_OverlayVertexBuffers[type]);
    ComPtr<ID3D11ShaderResourceView> oldTextureResourceView = std::move(m_OverlayTextureResourceViews[type]);
    SDL_AtomicUnlock(&m_OverlayLock);

    // If the overlay is disabled, we're done
    if (!overlayEnabled) {
        SDL_FreeSurface(newSurface);
        return;
    }

    // Create a texture with our pixel data
    SDL_assert(!SDL_MUSTLOCK(newSurface));
    SDL_assert(newSurface->format->format == SDL_PIXELFORMAT_ARGB8888);

    D3D11_TEXTURE2D_DESC texDesc = {};
    texDesc.Width = newSurface->w;
    texDesc.Height = newSurface->h;
    texDesc.MipLevels = 1;
    texDesc.ArraySize = 1;
    texDesc.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
    texDesc.SampleDesc.Count = 1;
    texDesc.SampleDesc.Quality = 0;
    texDesc.Usage = D3D11_USAGE_IMMUTABLE;
    texDesc.BindFlags = D3D11_BIND_SHADER_RESOURCE;
    texDesc.CPUAccessFlags = 0;
    texDesc.MiscFlags = 0;

    D3D11_SUBRESOURCE_DATA texData = {};
    texData.pSysMem = newSurface->pixels;
    texData.SysMemPitch = newSurface->pitch;

    ComPtr<ID3D11Texture2D> newTexture;
    hr = m_RenderDevice->CreateTexture2D(&texDesc, &texData, &newTexture);
    if (FAILED(hr)) {
        SDL_FreeSurface(newSurface);
        SDL_LogError(SDL_LOG_CATEGORY_APPLICATION,
                     "ID3D11Device::CreateTexture2D() failed: %x",
                     hr);
        return;
    }

    ComPtr<ID3D11ShaderResourceView> newTextureResourceView;
    hr = m_RenderDevice->CreateShaderResourceView((ID3D11Resource*)newTexture.Get(), nullptr, &newTextureResourceView);
    if (FAILED(hr)) {
        SDL_FreeSurface(newSurface);
        SDL_LogError(SDL_LOG_CATEGORY_APPLICATION,
                     "ID3D11Device::CreateShaderResourceView() failed: %x",
                     hr);
        return;
    }

    ComPtr<ID3D11Buffer> newVertexBuffer;
    if (!createOverlayVertexBuffer(type, newSurface->w, newSurface->h, newVertexBuffer)) {
        SDL_FreeSurface(newSurface);
        return;
    }

    // The surface is no longer required
    SDL_FreeSurface(newSurface);
    newSurface = nullptr;

    SDL_AtomicLock(&m_OverlayLock);
    m_OverlayVertexBuffers[type] = std::move(newVertexBuffer);
    m_OverlayTextures[type] = std::move(newTexture);
    m_OverlayTextureResourceViews[type] = std::move(newTextureResourceView);
    SDL_AtomicUnlock(&m_OverlayLock);
}

bool D3D11VARenderer::createOverlayVertexBuffer(Overlay::OverlayType type, int width, int height, ComPtr<ID3D11Buffer>& newVertexBuffer)
{
    SDL_FRect renderRect = {};

    if (type == Overlay::OverlayStatusUpdate) {
        // Bottom Left
        renderRect.x = 0;
        renderRect.y = 0;
    }
    else if (type == Overlay::OverlayDebug) {
        // Top left
        renderRect.x = 0;
        renderRect.y = m_DisplayHeight - height;
    }

    renderRect.w = width;
    renderRect.h = height;

    // Convert screen space to normalized device coordinates
    StreamUtils::screenSpaceToNormalizedDeviceCoords(&renderRect, m_DisplayWidth, m_DisplayHeight);

    VERTEX verts[] =
    {
        {renderRect.x, renderRect.y, 0, 1},
        {renderRect.x, renderRect.y+renderRect.h, 0, 0},
        {renderRect.x+renderRect.w, renderRect.y, 1, 1},
        {renderRect.x+renderRect.w, renderRect.y+renderRect.h, 1, 0},
    };

    D3D11_BUFFER_DESC vbDesc = {};
    vbDesc.ByteWidth = sizeof(verts);
    vbDesc.Usage = D3D11_USAGE_IMMUTABLE;
    vbDesc.BindFlags = D3D11_BIND_VERTEX_BUFFER;
    vbDesc.CPUAccessFlags = 0;
    vbDesc.MiscFlags = 0;
    vbDesc.StructureByteStride = sizeof(VERTEX);

    D3D11_SUBRESOURCE_DATA vbData = {};
    vbData.pSysMem = verts;

    HRESULT hr = m_RenderDevice->CreateBuffer(&vbDesc, &vbData, &newVertexBuffer);
    if (FAILED(hr)) {
        SDL_LogError(SDL_LOG_CATEGORY_APPLICATION,
                     "ID3D11Device::CreateBuffer() failed: %x",
                     hr);
        return false;
    }

    return true;
}

bool D3D11VARenderer::notifyWindowChanged(PWINDOW_STATE_CHANGE_INFO stateInfo)
{
    if (stateInfo->stateChangeFlags & WINDOW_STATE_CHANGE_DISPLAY) {
        int adapterIndex, outputIndex;
        if (!SDL_DXGIGetOutputInfo(stateInfo->displayIndex,
                                   &adapterIndex, &outputIndex)) {
            SDL_LogError(SDL_LOG_CATEGORY_APPLICATION,
                         "SDL_DXGIGetOutputInfo() failed: %s",
                         SDL_GetError());
            return false;
        }

        // If the window moved to a different GPU, recreate the renderer
        // to see if we can use that new GPU for decoding
        if (adapterIndex != m_AdapterIndex) {
            return false;
        }
        m_OutputIndex = outputIndex;

        // If an adapter was added or removed, we can't trust that our
        // old indexes are still valid for comparison.
        if (!m_Factory->IsCurrent()) {
            return false;
        }

        refreshOutput();

        // We've handled this state change
        stateInfo->stateChangeFlags &= ~WINDOW_STATE_CHANGE_DISPLAY;
    }

    if (stateInfo->stateChangeFlags & WINDOW_STATE_CHANGE_SIZE) {
        // Resize our swapchain and reconstruct size-dependent resources

        DXGI_SWAP_CHAIN_DESC1 swapchainDesc;
        m_SwapChain->GetDesc1(&swapchainDesc);

        // Lock the context to avoid concurrent rendering
        lockContext(this);

        m_DisplayWidth = stateInfo->width;
        m_DisplayHeight = stateInfo->height;

        // Release the video vertex buffer so we will upload a new one after resize
        m_VideoVertexBuffer.Reset();

        // Create new vertex buffers for active overlays
        SDL_AtomicLock(&m_OverlayLock);
        for (size_t i = 0; i < m_OverlayVertexBuffers.size(); i++) {
            if (!m_OverlayTextures[i]) {
                continue;
            }

            D3D11_TEXTURE2D_DESC textureDesc;
            m_OverlayTextures[i]->GetDesc(&textureDesc);
            createOverlayVertexBuffer((Overlay::OverlayType)i, textureDesc.Width, textureDesc.Height, m_OverlayVertexBuffers[i]);
        }
        SDL_AtomicUnlock(&m_OverlayLock);

        // We must release all references to the back buffer
        m_RenderTargetView.Reset();
        m_RenderDeviceContext->Flush();

        HRESULT hr = m_SwapChain->ResizeBuffers(0, stateInfo->width, stateInfo->height, DXGI_FORMAT_UNKNOWN, swapchainDesc.Flags);
        if (FAILED(hr)) {
            SDL_LogError(SDL_LOG_CATEGORY_APPLICATION,
                         "IDXGISwapChain::ResizeBuffers() failed: %x",
                         hr);
            unlockContext(this);
            return false;
        }

        // Reset swapchain-dependent resources (RTV, viewport, etc)
        if (!setupSwapchainDependentResources()) {
            unlockContext(this);
            return false;
        }

        unlockContext(this);

        // We've handled this state change
        stateInfo->stateChangeFlags &= ~WINDOW_STATE_CHANGE_SIZE;
    }

    // Check if we've handled all state changes
    return stateInfo->stateChangeFlags == 0;
}

bool D3D11VARenderer::checkDecoderSupport(IDXGIAdapter* adapter)
{
    HRESULT hr;
    Microsoft::WRL::ComPtr<ID3D11VideoDevice> videoDevice;

    DXGI_ADAPTER_DESC adapterDesc;
    hr = adapter->GetDesc(&adapterDesc);
    if (FAILED(hr)) {
        SDL_LogError(SDL_LOG_CATEGORY_APPLICATION,
                     "IDXGIAdapter::GetDesc() failed: %x",
                     hr);
        return false;
    }

    // Derive a ID3D11VideoDevice from our ID3D11Device.
    hr = m_RenderDevice.As(&videoDevice);
    if (FAILED(hr)) {
        SDL_LogError(SDL_LOG_CATEGORY_APPLICATION,
                     "ID3D11Device::QueryInterface(ID3D11VideoDevice) failed: %x",
                     hr);
        return false;
    }

    // Check if the format is supported by this decoder
    BOOL supported;
    switch (m_DecoderParams.videoFormat)
    {
    case VIDEO_FORMAT_H264:
        if (FAILED(videoDevice->CheckVideoDecoderFormat(&D3D11_DECODER_PROFILE_H264_VLD_NOFGT, DXGI_FORMAT_NV12, &supported))) {
            SDL_LogError(SDL_LOG_CATEGORY_APPLICATION,
                         "GPU doesn't support H.264 decoding");
            return false;
        }
        else if (!supported) {
            SDL_LogError(SDL_LOG_CATEGORY_APPLICATION,
                         "GPU doesn't support H.264 decoding to NV12 format");
            return false;
        }
        break;

    case VIDEO_FORMAT_H264_HIGH8_444:
        // Unsupported by DXVA
        return false;

    case VIDEO_FORMAT_H265:
        if (FAILED(videoDevice->CheckVideoDecoderFormat(&D3D11_DECODER_PROFILE_HEVC_VLD_MAIN, DXGI_FORMAT_NV12, &supported))) {
            SDL_LogError(SDL_LOG_CATEGORY_APPLICATION,
                         "GPU doesn't support HEVC decoding");
            return false;
        }
        else if (!supported) {
            SDL_LogError(SDL_LOG_CATEGORY_APPLICATION,
                         "GPU doesn't support HEVC decoding to NV12 format");
            return false;
        }
        break;

    case VIDEO_FORMAT_H265_MAIN10:
        if (FAILED(videoDevice->CheckVideoDecoderFormat(&D3D11_DECODER_PROFILE_HEVC_VLD_MAIN10, DXGI_FORMAT_P010, &supported))) {
            SDL_LogError(SDL_LOG_CATEGORY_APPLICATION,
                         "GPU doesn't support HEVC Main10 decoding");
            return false;
        }
        else if (!supported) {
            SDL_LogError(SDL_LOG_CATEGORY_APPLICATION,
                         "GPU doesn't support HEVC Main10 decoding to P010 format");
            return false;
        }
        break;

    case VIDEO_FORMAT_H265_REXT8_444:
        if (FAILED(videoDevice->CheckVideoDecoderFormat(&k_D3D11_DECODER_PROFILE_HEVC_VLD_MAIN_444, DXGI_FORMAT_AYUV, &supported)))
        {
            SDL_LogError(SDL_LOG_CATEGORY_APPLICATION,
                         "GPU doesn't support HEVC Main 444 8-bit decoding via D3D11VA");
            return false;
        }
        else if (!supported) {
            SDL_LogError(SDL_LOG_CATEGORY_APPLICATION,
                         "GPU doesn't support HEVC Main 444 8-bit decoding to AYUV format");
            return false;
        }
        break;

    case VIDEO_FORMAT_H265_REXT10_444:
        if (FAILED(videoDevice->CheckVideoDecoderFormat(&k_D3D11_DECODER_PROFILE_HEVC_VLD_MAIN10_444, DXGI_FORMAT_Y410, &supported))) {
            SDL_LogError(SDL_LOG_CATEGORY_APPLICATION,
                         "GPU doesn't support HEVC Main 444 10-bit decoding via D3D11VA");
            return false;
        }
        else if (!supported) {
            SDL_LogError(SDL_LOG_CATEGORY_APPLICATION,
                         "GPU doesn't support HEVC Main 444 10-bit decoding to Y410 format");
            return false;
        }
        break;

    case VIDEO_FORMAT_AV1_MAIN8:
        if (FAILED(videoDevice->CheckVideoDecoderFormat(&D3D11_DECODER_PROFILE_AV1_VLD_PROFILE0, DXGI_FORMAT_NV12, &supported))) {
            SDL_LogError(SDL_LOG_CATEGORY_APPLICATION,
                         "GPU doesn't support AV1 decoding");
            return false;
        }
        else if (!supported) {
            SDL_LogError(SDL_LOG_CATEGORY_APPLICATION,
                         "GPU doesn't support AV1 decoding to NV12 format");
            return false;
        }
        break;

    case VIDEO_FORMAT_AV1_MAIN10:
        if (FAILED(videoDevice->CheckVideoDecoderFormat(&D3D11_DECODER_PROFILE_AV1_VLD_PROFILE0, DXGI_FORMAT_P010, &supported))) {
            SDL_LogError(SDL_LOG_CATEGORY_APPLICATION,
                         "GPU doesn't support AV1 Main 10-bit decoding");
            return false;
        }
        else if (!supported) {
            SDL_LogError(SDL_LOG_CATEGORY_APPLICATION,
                         "GPU doesn't support AV1 Main 10-bit decoding to P010 format");
            return false;
        }
        break;

    case VIDEO_FORMAT_AV1_HIGH8_444:
        if (FAILED(videoDevice->CheckVideoDecoderFormat(&D3D11_DECODER_PROFILE_AV1_VLD_PROFILE1, DXGI_FORMAT_AYUV, &supported))) {
            SDL_LogError(SDL_LOG_CATEGORY_APPLICATION,
                         "GPU doesn't support AV1 High 444 8-bit decoding");
            return false;
        }
        else if (!supported) {
            SDL_LogError(SDL_LOG_CATEGORY_APPLICATION,
                         "GPU doesn't support AV1 High 444 8-bit decoding to AYUV format");
            return false;
        }
        break;

    case VIDEO_FORMAT_AV1_HIGH10_444:
        if (FAILED(videoDevice->CheckVideoDecoderFormat(&D3D11_DECODER_PROFILE_AV1_VLD_PROFILE1, DXGI_FORMAT_Y410, &supported))) {
            SDL_LogError(SDL_LOG_CATEGORY_APPLICATION,
                         "GPU doesn't support AV1 High 444 10-bit decoding");
            return false;
        }
        else if (!supported) {
            SDL_LogError(SDL_LOG_CATEGORY_APPLICATION,
                         "GPU doesn't support AV1 High 444 10-bit decoding to Y410 format");
            return false;
        }
        break;

    default:
        SDL_assert(false);
        return false;
    }

    if (DXUtil::isFormatHybridDecodedByHardware(m_DecoderParams.videoFormat, adapterDesc.VendorId, adapterDesc.DeviceId)) {
        SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION,
                    "GPU decoding for format %x is blocked due to hardware limitations",
                    m_DecoderParams.videoFormat);
        return false;
    }

    return true;
}

int D3D11VARenderer::getRendererAttributes()
{
    int attributes = 0;

    // This renderer supports HDR
    attributes |= RENDERER_ATTRIBUTE_HDR_SUPPORT;

    // This renderer requires frame pacing to synchronize with VBlank when we're in full-screen
    // In windowed mode, we will render as fast we can and DWM will grab whatever is latest at the
    // time unless the user opts for pacing. We will use pacing in full-screen mode and normal DWM
    // sequencing in full-screen desktop mode to behave similarly to the DXVA2 renderer.
    if ((SDL_GetWindowFlags(m_DecoderParams.window) & SDL_WINDOW_FULLSCREEN_DESKTOP) == SDL_WINDOW_FULLSCREEN) {
        attributes |= RENDERER_ATTRIBUTE_FORCE_PACING;
    }

    return attributes;
}

IFFmpegRenderer::PresentationMode D3D11VARenderer::getPresentationMode()
{
    return m_PresentationMode;
}

const char* D3D11VARenderer::getPresentationModeFallbackReason()
{
    return m_PresentationModeFallbackReason;
}

uint64_t D3D11VARenderer::popPresentAlignmentWaitUs()
{
    uint64_t waitUs = m_LastPresentAlignmentWaitUs;
    m_LastPresentAlignmentWaitUs = 0;

    // In classic present mode, keep Pacer's render-lead estimate including
    // the alignment spin, bit-identical to the pre-target-hold tuning.
    if (qEnvironmentVariableIntValue("MOONLIGHT_VRR_CLASSIC_PRESENT") != 0) {
        return 0;
    }

    return waitUs;
}

void D3D11VARenderer::setPresentTargetUs(uint64_t targetUs, bool catchUp, uint64_t alignBudgetUs)
{
    // The catch-up policy is fully encoded in the pacer's alignment budget
    // (a rush with no cadence slack arrives with ~0), so the flag itself
    // isn't consulted here.
    Q_UNUSED(catchUp);

    m_PresentTargetUs = targetUs;
    m_PresentAlignBudgetUs = alignBudgetUs;
}

uint64_t D3D11VARenderer::getLastPresentUs()
{
    return m_LastPresentUs;
}

int D3D11VARenderer::getDecoderCapabilities()
{
    return CAPABILITY_REFERENCE_FRAME_INVALIDATION_HEVC |
           CAPABILITY_REFERENCE_FRAME_INVALIDATION_AV1;
}

IFFmpegRenderer::InitFailureReason D3D11VARenderer::getInitFailureReason()
{
    // In the specific case where we found at least one D3D11 hardware device but none of the
    // enumerated devices have support for the specified codec, tell the FFmpeg decoder not to
    // bother trying other hwaccels. We don't want to try loading D3D9 if the device doesn't
    // even have hardware support for the codec.
    //
    // NB: We use feature level 11.0 support as a gate here because we want to avoid returning
    // this failure reason in cases where we might have an extremely old GPU with support for
    // DXVA2 on D3D9 but not D3D11VA on D3D11. I'm unsure if any such drivers/hardware exists,
    // but better be safe than sorry.
    //
    // NB2: We're also assuming that no GPU exists which lacks any D3D11 driver but has drivers
    // for non-DX APIs like Vulkan. I believe this is a Windows Logo requirement so it should be
    // safe to assume.
    //
    // NB3: Sigh, there *are* GPUs drivers with greater codec support available via Vulkan than
    // D3D11VA even when both D3D11 and Vulkan APIs are supported. This is the case for HEVC RExt
    // profiles that were not supported by Microsoft until the Windows 11 24H2 SDK. Don't report
    // that hardware support is missing for YUV444 profiles since the Vulkan driver may support it.
    if (m_DevicesWithFL11Support != 0 && m_DevicesWithCodecSupport == 0 && !(m_DecoderParams.videoFormat & VIDEO_FORMAT_MASK_YUV444)) {
        return InitFailureReason::NoHardwareSupport;
    }
    else {
        return InitFailureReason::Unknown;
    }
}

void D3D11VARenderer::lockContext(void *lock_ctx)
{
    auto me = (D3D11VARenderer*)lock_ctx;

    SDL_LockMutex(me->m_ContextLock);
}

void D3D11VARenderer::unlockContext(void *lock_ctx)
{
    auto me = (D3D11VARenderer*)lock_ctx;

    SDL_UnlockMutex(me->m_ContextLock);
}

bool D3D11VARenderer::setupRenderingResources()
{
    HRESULT hr;

    m_RenderDeviceContext->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);

    // We use a common vertex shader for all pixel shaders
    {
        QByteArray vertexShaderBytecode = Path::readDataFile("d3d11_vertex.fxc");

        ComPtr<ID3D11VertexShader> vertexShader;
        hr = m_RenderDevice->CreateVertexShader(vertexShaderBytecode.constData(), vertexShaderBytecode.length(), nullptr, &vertexShader);
        if (SUCCEEDED(hr)) {
            m_RenderDeviceContext->VSSetShader(vertexShader.Get(), nullptr, 0);
        }
        else {
            SDL_LogError(SDL_LOG_CATEGORY_APPLICATION,
                         "ID3D11Device::CreateVertexShader() failed: %x",
                         hr);
            return false;
        }

        const D3D11_INPUT_ELEMENT_DESC vertexDesc[] =
        {
            { "POSITION", 0, DXGI_FORMAT_R32G32_FLOAT, 0, 0, D3D11_INPUT_PER_VERTEX_DATA, 0 },
            { "TEXCOORD", 0, DXGI_FORMAT_R32G32_FLOAT, 0, 8, D3D11_INPUT_PER_VERTEX_DATA, 0 },
        };
        ComPtr<ID3D11InputLayout> inputLayout;
        hr = m_RenderDevice->CreateInputLayout(vertexDesc, ARRAYSIZE(vertexDesc), vertexShaderBytecode.constData(), vertexShaderBytecode.length(), &inputLayout);
        if (SUCCEEDED(hr)) {
            m_RenderDeviceContext->IASetInputLayout(inputLayout.Get());
        }
        else {
            SDL_LogError(SDL_LOG_CATEGORY_APPLICATION,
                         "ID3D11Device::CreateInputLayout() failed: %x",
                         hr);
            return false;
        }
    }

    {
        QByteArray overlayPixelShaderBytecode = Path::readDataFile("d3d11_overlay_pixel.fxc");

        hr = m_RenderDevice->CreatePixelShader(overlayPixelShaderBytecode.constData(), overlayPixelShaderBytecode.length(), nullptr, &m_OverlayPixelShader);
        if (FAILED(hr)) {
            SDL_LogError(SDL_LOG_CATEGORY_APPLICATION,
                         "ID3D11Device::CreatePixelShader() failed: %x",
                         hr);
            return false;
        }
    }

    for (int i = 0; i < PixelShaders::_COUNT; i++)
    {
        QByteArray videoPixelShaderBytecode = Path::readDataFile(k_VideoShaderNames[i]);

        hr = m_RenderDevice->CreatePixelShader(videoPixelShaderBytecode.constData(), videoPixelShaderBytecode.length(), nullptr, &m_VideoPixelShaders[i]);
        if (FAILED(hr)) {
            SDL_LogError(SDL_LOG_CATEGORY_APPLICATION,
                         "ID3D11Device::CreatePixelShader() failed: %x",
                         hr);
            return false;
        }
    }

    // We use a common sampler for all pixel shaders
    {
        D3D11_SAMPLER_DESC samplerDesc = {};
        samplerDesc.Filter = D3D11_FILTER_MIN_MAG_MIP_LINEAR;
        samplerDesc.AddressU = D3D11_TEXTURE_ADDRESS_CLAMP;
        samplerDesc.AddressV = D3D11_TEXTURE_ADDRESS_CLAMP;
        samplerDesc.AddressW = D3D11_TEXTURE_ADDRESS_CLAMP;
        samplerDesc.MipLODBias = 0.0f;
        samplerDesc.MaxAnisotropy = 1;
        samplerDesc.ComparisonFunc = D3D11_COMPARISON_ALWAYS;
        samplerDesc.MinLOD = 0.0f;
        samplerDesc.MaxLOD = D3D11_FLOAT32_MAX;

        ComPtr<ID3D11SamplerState> sampler;
        hr = m_RenderDevice->CreateSamplerState(&samplerDesc,  &sampler);
        if (SUCCEEDED(hr)) {
            m_RenderDeviceContext->PSSetSamplers(0, 1, sampler.GetAddressOf());
        }
        else {
            SDL_LogError(SDL_LOG_CATEGORY_APPLICATION,
                         "ID3D11Device::CreateSamplerState() failed: %x",
                         hr);
            return false;
        }
    }

    // We use a common index buffer for all geometry
    {
        const int indexes[] = {0, 1, 2, 3, 2, 1};
        D3D11_BUFFER_DESC indexBufferDesc = {};
        indexBufferDesc.ByteWidth = sizeof(indexes);
        indexBufferDesc.Usage = D3D11_USAGE_IMMUTABLE;
        indexBufferDesc.BindFlags = D3D11_BIND_INDEX_BUFFER;
        indexBufferDesc.CPUAccessFlags = 0;
        indexBufferDesc.MiscFlags = 0;
        indexBufferDesc.StructureByteStride = sizeof(int);

        D3D11_SUBRESOURCE_DATA indexBufferData = {};
        indexBufferData.pSysMem = indexes;
        indexBufferData.SysMemPitch = sizeof(int);

        ComPtr<ID3D11Buffer> indexBuffer;
        hr = m_RenderDevice->CreateBuffer(&indexBufferDesc, &indexBufferData, &indexBuffer);
        if (SUCCEEDED(hr)) {
            m_RenderDeviceContext->IASetIndexBuffer(indexBuffer.Get(), DXGI_FORMAT_R32_UINT, 0);
        }
        else {
            SDL_LogError(SDL_LOG_CATEGORY_APPLICATION,
                         "ID3D11Device::CreateBuffer() failed: %x",
                         hr);
            return false;
        }
    }

    // Create our overlay blend state
    {
        D3D11_BLEND_DESC blendDesc = {};
        blendDesc.AlphaToCoverageEnable = FALSE;
        blendDesc.IndependentBlendEnable = FALSE;
        blendDesc.RenderTarget[0].BlendEnable = TRUE;
        blendDesc.RenderTarget[0].SrcBlend = D3D11_BLEND_SRC_ALPHA;
        blendDesc.RenderTarget[0].DestBlend = D3D11_BLEND_INV_SRC_ALPHA;
        blendDesc.RenderTarget[0].BlendOp = D3D11_BLEND_OP_ADD;
        blendDesc.RenderTarget[0].SrcBlendAlpha = D3D11_BLEND_ONE;
        blendDesc.RenderTarget[0].DestBlendAlpha = D3D11_BLEND_ZERO;
        blendDesc.RenderTarget[0].BlendOpAlpha = D3D11_BLEND_OP_ADD;
        blendDesc.RenderTarget[0].RenderTargetWriteMask = D3D11_COLOR_WRITE_ENABLE_ALL;

        hr = m_RenderDevice->CreateBlendState(&blendDesc, &m_OverlayBlendState);
        if (FAILED(hr)) {
            SDL_LogError(SDL_LOG_CATEGORY_APPLICATION,
                         "ID3D11Device::CreateBlendState() failed: %x",
                         hr);
            return false;
        }
    }

    // Create and bind our video blend state
    {
        D3D11_BLEND_DESC blendDesc = {};
        blendDesc.AlphaToCoverageEnable = FALSE;
        blendDesc.IndependentBlendEnable = FALSE;
        blendDesc.RenderTarget[0].BlendEnable = FALSE;
        blendDesc.RenderTarget[0].RenderTargetWriteMask = D3D11_COLOR_WRITE_ENABLE_ALL;

        hr = m_RenderDevice->CreateBlendState(&blendDesc, &m_VideoBlendState);
        if (SUCCEEDED(hr)) {
            m_RenderDeviceContext->OMSetBlendState(m_VideoBlendState.Get(), nullptr, 0xffffffff);
        }
        else {
            SDL_LogError(SDL_LOG_CATEGORY_APPLICATION,
                         "ID3D11Device::CreateBlendState() failed: %x",
                         hr);
            return false;
        }
    }

    if (!setupSwapchainDependentResources()) {
        return false;
    }

    return true;
}

bool D3D11VARenderer::setupSwapchainDependentResources()
{
    HRESULT hr;

    // Create our render target view
    {
        ComPtr<ID3D11Resource> backBufferResource;
        hr = m_SwapChain->GetBuffer(0, IID_PPV_ARGS(&backBufferResource));
        if (FAILED(hr)) {
            SDL_LogError(SDL_LOG_CATEGORY_APPLICATION,
                         "IDXGISwapChain::GetBuffer() failed: %x",
                         hr);
            return false;
        }

        hr = m_RenderDevice->CreateRenderTargetView(backBufferResource.Get(), nullptr, &m_RenderTargetView);
        if (FAILED(hr)) {
            SDL_LogError(SDL_LOG_CATEGORY_APPLICATION,
                         "ID3D11Device::CreateRenderTargetView() failed: %x",
                         hr);
            return false;
        }
    }

    // Set a viewport that fills the window
    {
        D3D11_VIEWPORT viewport;

        viewport.TopLeftX = 0;
        viewport.TopLeftY = 0;
        viewport.Width = m_DisplayWidth;
        viewport.Height = m_DisplayHeight;
        viewport.MinDepth = 0;
        viewport.MaxDepth = 1;

        m_RenderDeviceContext->RSSetViewports(1, &viewport);
    }

    return true;
}

// NB: This can be called more than once (and with different frame dimensions!)
bool D3D11VARenderer::setupFrameRenderingResources(AVHWFramesContext* framesContext)
{
    auto d3d11vaFramesContext = (AVD3D11VAFramesContext*)framesContext->hwctx;

    // Open the decoder texture array on the renderer device if we're using separate devices
    if (m_DecodeDevice != m_RenderDevice) {
        ComPtr<IDXGIResource1> dxgiDecoderResource;

        HRESULT hr = d3d11vaFramesContext->texture_infos->texture->QueryInterface(IID_PPV_ARGS(&dxgiDecoderResource));
        if (FAILED(hr)) {
            SDL_LogError(SDL_LOG_CATEGORY_APPLICATION,
                         "ID3D11Texture2D::QueryInterface(IDXGIResource1) failed: %x",
                         hr);
            return false;
        }

        HANDLE sharedHandle;
        hr = dxgiDecoderResource->CreateSharedHandle(nullptr, DXGI_SHARED_RESOURCE_READ, nullptr, &sharedHandle);
        if (FAILED(hr)) {
            SDL_LogError(SDL_LOG_CATEGORY_APPLICATION,
                         "IDXGIResource1::CreateSharedHandle() failed: %x",
                         hr);
            return false;
        }

        hr = m_RenderDevice->OpenSharedResource1(sharedHandle, IID_PPV_ARGS(&m_RenderSharedTextureArray));
        CloseHandle(sharedHandle);
        if (FAILED(hr)) {
            SDL_LogError(SDL_LOG_CATEGORY_APPLICATION,
                         "ID3D11Device1::OpenSharedResource1() failed: %x",
                         hr);
            return false;
        }
    }
    else {
        d3d11vaFramesContext->texture_infos->texture->AddRef();
        m_RenderSharedTextureArray.Attach(d3d11vaFramesContext->texture_infos->texture);
    }

    // Query the format of the underlying texture array
    D3D11_TEXTURE2D_DESC textureDesc;
    m_RenderSharedTextureArray->GetDesc(&textureDesc);
    m_TextureFormat = textureDesc.Format;

    if (m_BindDecoderOutputTextures) {
        // Create SRVs for all textures in the decoder pool
        if (!setupTexturePoolViews(framesContext)) {
            return false;
        }
    }
    else {
        // Create our internal texture to copy and render
        if (!setupVideoTexture(framesContext)) {
            return false;
        }
    }

    return true;
}

std::vector<DXGI_FORMAT> D3D11VARenderer::getVideoTextureSRVFormats()
{
    if (m_DecoderParams.videoFormat & VIDEO_FORMAT_MASK_YUV444) {
        // YUV 4:4:4 formats don't use a second SRV
        return { (m_DecoderParams.videoFormat & VIDEO_FORMAT_MASK_10BIT) ?
                    DXGI_FORMAT_R10G10B10A2_UNORM : DXGI_FORMAT_R8G8B8A8_UNORM };
    }
    else if (m_DecoderParams.videoFormat & VIDEO_FORMAT_MASK_10BIT) {
        return { DXGI_FORMAT_R16_UNORM, DXGI_FORMAT_R16G16_UNORM };
    }
    else {
        return { DXGI_FORMAT_R8_UNORM, DXGI_FORMAT_R8G8_UNORM };
    }
}

bool D3D11VARenderer::createVideoTextureSRV(ID3D11Resource* texture,
                                            D3D11_SRV_DIMENSION viewDimension,
                                            UINT arraySlice,
                                            DXGI_FORMAT srvFormat,
                                            UINT planeSlice,
                                            ComPtr<ID3D11ShaderResourceView>& srv)
{
    HRESULT hr;

    if (m_TextureFormat == DXGI_FORMAT_NV12 ||
            m_TextureFormat == DXGI_FORMAT_P010 ||
            m_TextureFormat == DXGI_FORMAT_P016) {
        D3D11_SHADER_RESOURCE_VIEW_DESC1 srvDesc = {};
        srvDesc.Format = srvFormat;
        srvDesc.ViewDimension = viewDimension;

        if (viewDimension == D3D11_SRV_DIMENSION_TEXTURE2DARRAY) {
            srvDesc.Texture2DArray.MostDetailedMip = 0;
            srvDesc.Texture2DArray.MipLevels = 1;
            srvDesc.Texture2DArray.FirstArraySlice = arraySlice;
            srvDesc.Texture2DArray.ArraySize = 1;
            srvDesc.Texture2DArray.PlaneSlice = planeSlice;
        }
        else {
            SDL_assert(viewDimension == D3D11_SRV_DIMENSION_TEXTURE2D);
            srvDesc.Texture2D.MostDetailedMip = 0;
            srvDesc.Texture2D.MipLevels = 1;
            srvDesc.Texture2D.PlaneSlice = planeSlice;
        }

        ComPtr<ID3D11ShaderResourceView1> srv1;
        hr = m_RenderDevice->CreateShaderResourceView1(texture, &srvDesc, &srv1);
        if (SUCCEEDED(hr)) {
            hr = srv1.As(&srv);
        }
    }
    else {
        D3D11_SHADER_RESOURCE_VIEW_DESC srvDesc = {};
        srvDesc.Format = srvFormat;
        srvDesc.ViewDimension = viewDimension;

        if (viewDimension == D3D11_SRV_DIMENSION_TEXTURE2DARRAY) {
            srvDesc.Texture2DArray.MostDetailedMip = 0;
            srvDesc.Texture2DArray.MipLevels = 1;
            srvDesc.Texture2DArray.FirstArraySlice = arraySlice;
            srvDesc.Texture2DArray.ArraySize = 1;
        }
        else {
            SDL_assert(viewDimension == D3D11_SRV_DIMENSION_TEXTURE2D);
            srvDesc.Texture2D.MostDetailedMip = 0;
            srvDesc.Texture2D.MipLevels = 1;
        }

        hr = m_RenderDevice->CreateShaderResourceView(texture, &srvDesc, &srv);
    }

    if (FAILED(hr)) {
        SDL_LogError(SDL_LOG_CATEGORY_APPLICATION,
                     "ID3D11Device::CreateShaderResourceView%s() failed: %x resource-format=%u srv-format=%u plane=%u view=%u array-slice=%u video-format=0x%x",
                     (m_TextureFormat == DXGI_FORMAT_NV12 ||
                      m_TextureFormat == DXGI_FORMAT_P010 ||
                      m_TextureFormat == DXGI_FORMAT_P016) ? "1" : "",
                     hr,
                     m_TextureFormat,
                     srvFormat,
                     planeSlice,
                     viewDimension,
                     arraySlice,
                     m_DecoderParams.videoFormat);
        return false;
    }

    return true;
}

bool D3D11VARenderer::setupVideoTexture(AVHWFramesContext* framesContext)
{
    SDL_assert(!m_BindDecoderOutputTextures);

    HRESULT hr;
    D3D11_TEXTURE2D_DESC texDesc = {};

    texDesc.Width = framesContext->width;
    texDesc.Height = framesContext->height;
    texDesc.MipLevels = 1;
    texDesc.ArraySize = 1;
    texDesc.Format = m_TextureFormat;
    texDesc.SampleDesc.Quality = 0;
    texDesc.SampleDesc.Count = 1;
    texDesc.Usage = D3D11_USAGE_DEFAULT;
    texDesc.BindFlags = D3D11_BIND_SHADER_RESOURCE;
    texDesc.CPUAccessFlags = 0;
    texDesc.MiscFlags = 0;

    hr = m_RenderDevice->CreateTexture2D(&texDesc, nullptr, &m_VideoTexture);
    if (FAILED(hr)) {
        SDL_LogError(SDL_LOG_CATEGORY_APPLICATION,
                     "ID3D11Device::CreateTexture2D() failed: %x",
                     hr);
        return false;
    }

    // We will only have one set of SRVs
    m_VideoTextureResourceViews.resize(1);

    size_t srvIndex = 0;
    for (DXGI_FORMAT srvFormat : getVideoTextureSRVFormats()) {
        SDL_assert(srvIndex < m_VideoTextureResourceViews[0].size());

        if (!createVideoTextureSRV(m_VideoTexture.Get(),
                                   D3D11_SRV_DIMENSION_TEXTURE2D,
                                   0,
                                   srvFormat,
                                   (UINT)srvIndex,
                                   m_VideoTextureResourceViews[0][srvIndex])) {
            return false;
        }

        srvIndex++;
    }

    return true;
}

bool D3D11VARenderer::setupTexturePoolViews(AVHWFramesContext* framesContext)
{
    AVD3D11VAFramesContext* d3d11vaFramesContext = (AVD3D11VAFramesContext*)framesContext->hwctx;

    SDL_assert(m_BindDecoderOutputTextures);

    m_VideoTextureResourceViews.resize(framesContext->initial_pool_size);

    // Create luminance and chrominance SRVs for each texture in the pool
    for (int i = 0; i < framesContext->initial_pool_size; i++) {
        // Our rendering logic depends on the texture index working to map into our SRV array
        SDL_assert(i == d3d11vaFramesContext->texture_infos[i].index);

        size_t srvIndex = 0;
        for (DXGI_FORMAT srvFormat : getVideoTextureSRVFormats()) {
            SDL_assert(srvIndex < m_VideoTextureResourceViews[i].size());

            if (!createVideoTextureSRV(m_RenderSharedTextureArray.Get(),
                                       D3D11_SRV_DIMENSION_TEXTURE2DARRAY,
                                       d3d11vaFramesContext->texture_infos[i].index,
                                       srvFormat,
                                       (UINT)srvIndex,
                                       m_VideoTextureResourceViews[i][srvIndex])) {
                return false;
            }

            srvIndex++;
        }
    }

    return true;
}
