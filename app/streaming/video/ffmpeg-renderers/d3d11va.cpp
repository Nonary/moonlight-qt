// For D3D11_DECODER_PROFILE values
#include <initguid.h>

#include "d3d11va.h"
#include "dxutil.h"
#include "path.h"
#include "utils.h"

#include "streaming/streamutils.h"
#include "streaming/session.h"

#include <SDL_syswm.h>
#include <Limelight.h>

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

typedef struct _BFI_CONST_BUF
{
    float referenceWhiteNits;
    float padding[3];

    // Rows of the source RGB -> BT.2020 RGB matrix. Keeping these as rows
    // avoids relying on HLSL's matrix-major packing rules.
    float sourceToBt2020[3][4];
} BFI_CONST_BUF;
static_assert(sizeof(BFI_CONST_BUF) % 16 == 0,
              "Constant buffer sizes must be a multiple of 16");

static const std::array<const char*, D3D11VARenderer::PixelShaders::_COUNT> k_VideoShaderNames =
{
    "d3d11_yuv420_pixel.fxc",
    "d3d11_ayuv_pixel.fxc",
    "d3d11_y410_pixel.fxc",
    "d3d11_yuv420_bfi_pixel.fxc",
    "d3d11_ayuv_bfi_pixel.fxc",
    "d3d11_y410_bfi_pixel.fxc",
};

static bool translateQpcToPacingTime(LARGE_INTEGER syncQpcTime,
                                     uint64_t& translatedTimeUs)
{
    if (syncQpcTime.QuadPart <= 0) {
        return false;
    }

    LARGE_INTEGER qpcNow;
    LARGE_INTEGER qpcFrequency;
    if (!QueryPerformanceCounter(&qpcNow) ||
            !QueryPerformanceFrequency(&qpcFrequency) ||
            qpcFrequency.QuadPart <= 0 ||
            qpcNow.QuadPart < syncQpcTime.QuadPart) {
        return false;
    }

    const uint64_t nowUs = LiGetMicroseconds();
    const uint64_t ageTicks = static_cast<uint64_t>(
        qpcNow.QuadPart - syncQpcTime.QuadPart);
    const uint64_t ticksPerSecond = static_cast<uint64_t>(
        qpcFrequency.QuadPart);
    const uint64_t ageUs =
        (ageTicks / ticksPerSecond) * 1000000ULL +
        (ageTicks % ticksPerSecond) * 1000000ULL / ticksPerSecond;
    translatedTimeUs = nowUs > ageUs ? nowUs - ageUs : 0;
    return true;
}

// The BFI swapchain is declared as HDR10 BT.2020. SDR video is normally
// BT.709, but keep the other matrices available for streams whose metadata
// identifies SD or BT.2020 primaries.
static const float k_BfiSourceToBt2020[3][3][3] =
{
    {
        { 0.5952542f, 0.3493140f, 0.0554320f },
        { 0.0812437f, 0.8915034f, 0.0272521f },
        { 0.0155123f, 0.0819116f, 0.9025760f },
    },
    {
        { 0.6274039f, 0.3292830f, 0.0433131f },
        { 0.0690973f, 0.9195404f, 0.0113623f },
        { 0.0163914f, 0.0880133f, 0.8955953f },
    },
    {
        { 1.0f, 0.0f, 0.0f },
        { 0.0f, 1.0f, 0.0f },
        { 0.0f, 0.0f, 1.0f },
    },
};

// Keep the established BFI luminance targets: 600 nits for the visible frame
// and 300 nits for recovery across the 50% black/video duty cycle.
static constexpr float k_BfiReferenceWhiteNits = 600.0f;

static int bfiColorSpaceIndex(int colorSpace)
{
    switch (colorSpace) {
    case COLORSPACE_REC_601:
        return 0;
    case COLORSPACE_REC_2020:
        return 2;
    case COLORSPACE_REC_709:
    default:
        return 1;
    }
}

static int bfiColorSpaceIndex(const AVFrame* frame, int fallbackColorSpace)
{
    // Primaries describe the RGB gamut. The colorspace field describes the
    // YUV-to-RGB matrix, so prefer primaries for the gamut conversion and use
    // the matrix as the fallback when stream metadata is incomplete.
    switch (frame->color_primaries) {
    case AVCOL_PRI_BT470BG:
    case AVCOL_PRI_SMPTE170M:
        return 0;
    case AVCOL_PRI_BT709:
        return 1;
    case AVCOL_PRI_BT2020:
        return 2;
    default:
        return bfiColorSpaceIndex(fallbackColorSpace);
    }
}

D3D11VARenderer::D3D11VARenderer(int decoderSelectionPass)
    : IFFmpegRenderer(RendererType::D3D11VA),
      m_DecoderSelectionPass(decoderSelectionPass),
      m_DevicesWithFL11Support(0),
      m_DevicesWithCodecSupport(0),
      m_AdapterIndex(-1),
      m_RenderAdapterIndex(-1),
      m_LastColorTrc(AVCOL_TRC_UNSPECIFIED),
      m_AllowTearing(false),
      m_BlackFrameInsertionActive(false),
      m_BlackFrameInsertionBlackPresented(false),
      m_BlackFrameInsertionDropRecovery(false),
      m_BlackFrameInsertionForceTearing(false),
      m_BlackFrameInsertionLeadTimeUs(0),
      m_BlackFrameInsertionCacheValid(false),
      m_BlackFrameInsertionCacheWidth(0),
      m_BlackFrameInsertionCacheHeight(0),
      m_BlackFrameInsertionRestoreRejects(0),
      m_VrrBorderlessFlipModel(false),
      m_VrrSwapChainAllowsTearing(false),
      m_VrrSuspended(false),
      m_VrrFallbackReason(VrrFallbackReason::InitializationFailed),
      m_VrrFramePrepared(false),
      m_VrrContextLocked(false),
      m_VrrPresentReadyFenceValue(0),
      m_VrrPresentReadyFenceEvent(nullptr),
      m_VrrPresentReadyAvailable(false),
      m_OverlayLock(0),
      m_HwDeviceContext(nullptr)
{
    m_ContextLock = SDL_CreateMutex();
    m_PresentationLock = SDL_CreateMutex();
    DwmEnableMMCSS(TRUE);
}

bool D3D11VARenderer::updateBfiConstants(const AVFrame* frame)
{
    if (frame == nullptr ||
            m_BlackFrameInsertionBrightConstants == nullptr ||
            m_BlackFrameInsertionRecoveryConstants == nullptr) {
        return false;
    }

    const int colorSpace = bfiColorSpaceIndex(frame, getFrameColorspace(frame));
    if (colorSpace == m_LastBfiColorSpace) {
        return true;
    }

    const auto updateBuffer = [&](ID3D11Buffer* buffer,
                                  float referenceWhiteNits) {
        D3D11_MAPPED_SUBRESOURCE mapped = {};
        HRESULT hr = m_RenderDeviceContext->Map(
            buffer, 0, D3D11_MAP_WRITE_DISCARD, 0, &mapped);
        if (FAILED(hr)) {
            SDL_LogError(SDL_LOG_CATEGORY_APPLICATION,
                         "ID3D11DeviceContext::Map() failed for BFI constants: %x",
                         hr);
            return false;
        }

        auto* constants = static_cast<BFI_CONST_BUF*>(mapped.pData);
        constants->referenceWhiteNits = referenceWhiteNits;
        constants->padding[0] = 0.0f;
        constants->padding[1] = 0.0f;
        constants->padding[2] = 0.0f;
        for (int row = 0; row < 3; row++) {
            for (int column = 0; column < 3; column++) {
                constants->sourceToBt2020[row][column] =
                    k_BfiSourceToBt2020[colorSpace][row][column];
            }
            constants->sourceToBt2020[row][3] = 0.0f;
        }

        m_RenderDeviceContext->Unmap(buffer, 0);
        return true;
    };

    if (!updateBuffer(m_BlackFrameInsertionBrightConstants.Get(),
                      k_BfiReferenceWhiteNits) ||
            !updateBuffer(m_BlackFrameInsertionRecoveryConstants.Get(),
                          k_BfiReferenceWhiteNits * 0.5f)) {
        return false;
    }

    m_LastBfiColorSpace = colorSpace;
    return true;
}

D3D11VARenderer::~D3D11VARenderer()
{
    DwmEnableMMCSS(FALSE);

    // The VRR worker may be cancelled after preparation but before Present.
    // Release the retained D3D context lock before destroying it or any
    // back-buffer objects.
    cancelFrame();
    SDL_DestroyMutex(m_PresentationLock);
    SDL_DestroyMutex(m_ContextLock);

    m_VideoVertexBuffer.Reset();
    m_BlackFrameInsertionFullscreenVertexBuffer.Reset();
    m_BlackFrameInsertionBrightConstants.Reset();
    m_BlackFrameInsertionRecoveryConstants.Reset();
    m_BlackFrameInsertionDimPixelShader.Reset();
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
    m_BfiOverlayPixelShader.Reset();

    m_OverlayBlendState.Reset();
    m_VideoBlendState.Reset();

    m_DecodeD2RFence.Reset();
    m_DecodeR2DFence.Reset();
    m_RenderD2RFence.Reset();
    m_RenderR2DFence.Reset();

    m_VrrPresentReadyFence.Reset();
    if (m_VrrPresentReadyFenceEvent != nullptr) {
        CloseHandle(m_VrrPresentReadyFenceEvent);
        m_VrrPresentReadyFenceEvent = nullptr;
    }

    m_BlackFrameInsertionVideoTextureSrv.Reset();
    m_BlackFrameInsertionVideoTexture.Reset();
    m_RenderTargetView.Reset();
    m_RenderTargetTexture.Reset();
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
        // Also bind SRVs when using separate decoding and rendering
        // devices as this improves render times by about 2x on my
        // Ryzen 3300U system. The fences we use between decoding
        // and rendering contexts should hopefully avoid any of the
        // synchronization issues we've seen between decoder and SRVs.
        m_BindDecoderOutputTextures = adapterDesc.VendorId == 0x8086 || separateDevices;
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

    m_RenderAdapterIndex = adapterIndex;
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
    swapChainDesc.AlphaMode = DXGI_ALPHA_MODE_UNSPECIFIED;
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
    // the non-blocking present we expect.
    //
    // NB: 3 total buffers seems sufficient on NVIDIA hardware but
    // causes performance issues (buffer starvation) on AMD GPUs.
    swapChainDesc.BufferCount = 3 + 1 + 1;

    // Use the current window size as the swapchain size
    SDL_GetWindowSize(params->window, (int*)&swapChainDesc.Width, (int*)&swapChainDesc.Height);

    m_DisplayWidth = swapChainDesc.Width;
    m_DisplayHeight = swapChainDesc.Height;

    if ((params->videoFormat & VIDEO_FORMAT_MASK_10BIT) || params->enableBlackFrameInsertion) {
        swapChainDesc.Format = DXGI_FORMAT_R10G10B10A2_UNORM;
    }
    else {
        swapChainDesc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    }

    initializeVrrPresentationState(&swapChainDesc);

    // DXVA2 may let us take over for FSE V-sync off cases. However, if we don't have DXGI_FEATURE_PRESENT_ALLOW_TEARING
    // then we should not attempt to do this unless there's no other option (HDR, DXVA2 failed in pass 1, etc).
    if (!m_AllowTearing && !params->enableVsync && m_DecoderSelectionPass == 0 && !(params->videoFormat & VIDEO_FORMAT_MASK_10BIT) &&
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

    // Determine VRR eligibility from the created swapchain rather than the
    // requested descriptor, since the runtime may normalize it.
    refreshVrrDisplayState();

    if (!initializeBlackFrameInsertion()) {
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

void D3D11VARenderer::renderFrame(AVFrame* frame)
{
    const bool frameIndependentRefresh = needsFrameIndependentRefresh();
    if (frameIndependentRefresh) {
        SDL_LockMutex(m_PresentationLock);
    }

    // Acquire the context lock for rendering to prevent concurrent
    // access from inside FFmpeg's decoding code
    if (m_DecodeDevice == m_RenderDevice) {
        lockContext(this);
    }

    // Keep the existing fixed/unpaced behavior intact while sharing the same
    // preparation and final Present helpers used by the opt-in VRR backend.
    UINT flags = 0;
    if (m_AllowTearing) {
        SDL_assert(!m_DecoderParams.enableVsync);

        // If tearing is allowed, use DXGI_PRESENT_ALLOW_TEARING with syncInterval 0.
        // It is not valid to use any other syncInterval values in tearing mode.
        flags = DXGI_PRESENT_ALLOW_TEARING;
    }

    bool prepared = prepareFrameForPresent(frame);
    HRESULT hr = E_FAIL;
    if (prepared && frameIndependentRefresh) {
        // The fixed-VSync carrier owns presentation. This decoded frame only
        // replaces the cached lit image, so irregular source delivery cannot
        // alter the black/video duty cycle.
        hr = S_OK;
    }
    else if (prepared && m_BlackFrameInsertionActive) {
        // Adaptive BFI is retained only as a defensive legacy fallback. New
        // sessions deliberately select the fixed carrier below.
        hr = presentBlackFrame(1, 0);
        if (SUCCEEDED(hr)) {
            hr = restoreBlackFrameInsertionVideo() ?
                m_SwapChain->Present(1, 0) : E_FAIL;
        }
    }
    else if (prepared) {
        hr = presentPreparedFrame(flags);
    }

    if (m_DecodeDevice == m_RenderDevice) {
        // Release the context lock
        unlockContext(this);
    }
    if (frameIndependentRefresh) {
        SDL_UnlockMutex(m_PresentationLock);
    }

    if (FAILED(hr)) {
        if (prepared) {
            SDL_LogError(SDL_LOG_CATEGORY_APPLICATION,
                         "IDXGISwapChain::Present() failed: %x",
                         hr);
        }
        else {
            SDL_LogError(SDL_LOG_CATEGORY_APPLICATION,
                         "D3D11 frame preparation failed");
        }

        // The card may have been removed or crashed. Reset the decoder.
        queueRenderDeviceReset();
        return;
    }
}

bool D3D11VARenderer::needsFrameIndependentRefresh() const
{
    return m_BlackFrameInsertionActive &&
        !m_DecoderParams.enableVrr;
}

bool D3D11VARenderer::renderFrameIndependentRefresh()
{
    if (!needsFrameIndependentRefresh()) {
        return false;
    }

    // Advance one half of the fixed carrier per render-loop iteration. This
    // lets the loop sample a frame that finishes decoding during black before
    // it commits the following lit slot, avoiding an unnecessary extra dupe.
    SDL_LockMutex(m_PresentationLock);

    HRESULT hr = E_FAIL;
    lockContext(this);
    if (m_SwapChain != nullptr) {
        if (m_BlackFrameInsertionBlackPresented &&
                restoreBlackFrameInsertionVideo()) {
            hr = S_OK;
        }
        else if (m_RenderDeviceContext != nullptr &&
                m_RenderTargetView != nullptr) {
            // Either this is the black half of the carrier, or the video
            // restore was rejected because the cache has no complete copy of
            // the current render target (for example immediately after a
            // resize). Presenting black preserves the carrier cadence; the
            // next prepared frame republishes the cache and relights it.
            const float black[4] = {0.0f, 0.0f, 0.0f, 1.0f};
            m_RenderDeviceContext->ClearRenderTargetView(
                m_RenderTargetView.Get(), black);
            hr = S_OK;
        }
    }
    unlockContext(this);

    // Never retain the shared D3D context mutex through the blocking V-Sync
    // wait. Decode remains free to make progress during either carrier phase.
    if (hr == S_OK) {
        hr = m_SwapChain->Present(1, 0);
    }

    SDL_UnlockMutex(m_PresentationLock);

    if (hr == DXGI_STATUS_OCCLUDED) {
        // Keep the renderer's logical phase synchronized with the render
        // thread even though DWM did not make the occluded Present visible.
        // The phase itself is irrelevant while hidden, but matching the
        // successful-call contract prevents content updates on bright phases
        // after the window becomes visible again.
        m_BlackFrameInsertionBlackPresented =
            !m_BlackFrameInsertionBlackPresented;
        SDL_Delay(10);
        return true;
    }
    if (hr != S_OK) {
        SDL_LogError(SDL_LOG_CATEGORY_APPLICATION,
                     "D3D11 fixed BFI carrier Present() failed: phase=%s hr=%x",
                     m_BlackFrameInsertionBlackPresented ? "video" : "black",
                     hr);
        m_BlackFrameInsertionActive = false;
        queueRenderDeviceReset();
        return false;
    }

    m_BlackFrameInsertionBlackPresented =
        !m_BlackFrameInsertionBlackPresented;
    return true;
}

void D3D11VARenderer::cleanupRenderContext()
{
    if (!needsFrameIndependentRefresh() ||
            !m_BlackFrameInsertionBlackPresented) {
        return;
    }

    // Shutdown can arrive after the black half of the carrier. Complete that
    // pair synchronously so the swapchain never remains black while teardown
    // and HDR desktop restoration finish.
    SDL_LockMutex(m_PresentationLock);
    lockContext(this);
    const bool restored = restoreBlackFrameInsertionVideo();
    unlockContext(this);
    const HRESULT hr = restored && m_SwapChain != nullptr ?
        m_SwapChain->Present(1, 0) : E_FAIL;
    if (hr == S_OK) {
        m_BlackFrameInsertionBlackPresented = false;
    }
    else if (hr != DXGI_STATUS_OCCLUDED) {
        SDL_LogWarn(SDL_LOG_CATEGORY_APPLICATION,
                    "D3D11 fixed BFI shutdown restore failed: %x", hr);
    }
    SDL_UnlockMutex(m_PresentationLock);
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
    m_RenderDeviceContext->PSSetShader(
        m_BlackFrameInsertionActive ? m_BfiOverlayPixelShader.Get() : m_OverlayPixelShader.Get(),
        nullptr, 0);
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
            m_RenderDeviceContext->PSSetShader(m_VideoPixelShaders[
                m_BlackFrameInsertionActive && frame->color_trc != AVCOL_TRC_SMPTE2084 ?
                    PixelShaders::BFI_AYUV : PixelShaders::GENERIC_AYUV].Get(), nullptr, 0);
            break;
        case DXGI_FORMAT_Y410:
            m_RenderDeviceContext->PSSetShader(m_VideoPixelShaders[
                m_BlackFrameInsertionActive && frame->color_trc != AVCOL_TRC_SMPTE2084 ?
                    PixelShaders::BFI_Y410 : PixelShaders::GENERIC_Y410].Get(), nullptr, 0);
            break;
        default:
            SDL_assert(false);
        }
    }
    else {
        // We'll need to use the generic 4:2:0 shader for this colorspace and color range combo
        m_RenderDeviceContext->PSSetShader(m_VideoPixelShaders[
            m_BlackFrameInsertionActive && frame->color_trc != AVCOL_TRC_SMPTE2084 ?
                PixelShaders::BFI_YUV_420 : PixelShaders::GENERIC_YUV_420].Get(), nullptr, 0);
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

        bool acquiredContextLock = false;
        if (!m_VrrContextLocked) {
            lockContext(this);
            acquiredContextLock = true;
        }
        if (SUCCEEDED(m_DecodeDeviceContext->Signal(m_DecodeD2RFence.Get(), m_D2RFenceValue))) {
            m_RenderDeviceContext->Wait(m_RenderD2RFence.Get(), m_D2RFenceValue++);
        }
        if (acquiredContextLock) {
            unlockContext(this);
        }
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
            bool acquiredContextLock = false;
            if (!m_VrrContextLocked) {
                lockContext(this);
                acquiredContextLock = true;
            }
            SDL_assert(m_R2DFenceValue > 0);
            m_DecodeDeviceContext->Wait(m_DecodeR2DFence.Get(), m_R2DFenceValue - 1);
            if (acquiredContextLock) {
                unlockContext(this);
            }
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

        // If an adapter was added or removed, we can't trust that our
        // old indexes are still valid for comparison.
        if (!m_Factory->IsCurrent()) {
            return false;
        }

        // A same-GPU display move keeps this renderer alive. Serialize the
        // refreshed swapchain eligibility against the context lock the VRR
        // worker retains from preparation through Present.
        SDL_LockMutex(m_PresentationLock);
        lockContext(this);
        refreshVrrDisplayState();
        const bool wasBlackFrameInsertionActive =
            m_BlackFrameInsertionActive;
        m_BlackFrameInsertionActive = false;
        if (!initializeBlackFrameInsertion()) {
            unlockContext(this);
            SDL_UnlockMutex(m_PresentationLock);
            return false;
        }
        if (m_BlackFrameInsertionActive !=
                wasBlackFrameInsertionActive) {
            unlockContext(this);
            SDL_UnlockMutex(m_PresentationLock);
            return false;
        }
        unlockContext(this);
        SDL_UnlockMutex(m_PresentationLock);

        // We've handled this state change
        stateInfo->stateChangeFlags &= ~WINDOW_STATE_CHANGE_DISPLAY;
    }

    if (stateInfo->stateChangeFlags & WINDOW_STATE_CHANGE_SIZE) {
        // Resize our swapchain and reconstruct size-dependent resources

        SDL_LockMutex(m_PresentationLock);

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

        // The outgoing BFI cache belongs to the outgoing backbuffer. Revoke
        // its restore eligibility before any release so no restore path can
        // copy stale or uninitialized data into the replacement target.
        m_BlackFrameInsertionCacheValid = false;

        // We must release all references to the back buffer, including any
        // render target binding still held by the context state.
        m_RenderDeviceContext->OMSetRenderTargets(0, nullptr, nullptr);
        m_BlackFrameInsertionVideoTextureSrv.Reset();
        m_BlackFrameInsertionVideoTexture.Reset();
        m_RenderTargetView.Reset();
        m_RenderTargetTexture.Reset();
        m_RenderDeviceContext->Flush();

        HRESULT hr = m_SwapChain->ResizeBuffers(0, stateInfo->width, stateInfo->height, DXGI_FORMAT_UNKNOWN, swapchainDesc.Flags);
        if (FAILED(hr)) {
            SDL_LogError(SDL_LOG_CATEGORY_APPLICATION,
                         "IDXGISwapChain::ResizeBuffers() failed: %x",
                         hr);
            unlockContext(this);
            SDL_UnlockMutex(m_PresentationLock);
            return false;
        }

        // Reset swapchain-dependent resources (RTV, viewport, etc)
        if (!setupSwapchainDependentResources()) {
            unlockContext(this);
            SDL_UnlockMutex(m_PresentationLock);
            return false;
        }

        // A same-monitor mode switch can arrive only as SIZE_CHANGED, so
        // re-evaluate VRR eligibility from the resized swapchain.
        refreshVrrDisplayState();

        unlockContext(this);
        SDL_UnlockMutex(m_PresentationLock);

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

    // This renderer requires frame pacing to synchronize with VBlank when we're in full-screen.
    // In windowed mode, we will render as fast we can and DWM will grab whatever is latest at the
    // time unless the user opts for pacing. We will use pacing in full-screen mode and normal DWM
    // sequencing in full-screen desktop mode to behave similarly to the DXVA2 renderer.
    if ((SDL_GetWindowFlags(m_DecoderParams.window) & SDL_WINDOW_FULLSCREEN_DESKTOP) == SDL_WINDOW_FULLSCREEN) {
        attributes |= RENDERER_ATTRIBUTE_FORCE_PACING;
    }

    return attributes;
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

void D3D11VARenderer::initializeVrrPresentationState(DXGI_SWAP_CHAIN_DESC1* swapChainDesc)
{
    // Preserve the legacy non-VSync path while also creating an
    // allow-tearing flip swapchain for an explicitly requested VRR session.
    // The latter is required even though the session's effective V-Sync is
    // true: the VRR worker always uses immediate presentation and owns the
    // complete mathematical pacing policy.
    if (!m_DecoderParams.enableVsync || m_DecoderParams.enableVrr) {
        BOOL allowTearing = FALSE;
        HRESULT hr = m_Factory->CheckFeatureSupport(DXGI_FEATURE_PRESENT_ALLOW_TEARING,
                                                    &allowTearing,
                                                    sizeof(allowTearing));
        if (SUCCEEDED(hr) && allowTearing) {
            // VRR eligibility gates on the swapchain carrying this flag, which
            // is only requested here when the feature query succeeded.  Never
            // set it unconditionally.
            swapChainDesc->Flags |= DXGI_SWAP_CHAIN_FLAG_ALLOW_TEARING;

            // Do not alter legacy V-Sync semantics.  Only the existing
            // non-VSync path uses this flag from renderFrame().
            if (!m_DecoderParams.enableVsync) {
                m_AllowTearing = true;
            }
        }
        else if (!m_DecoderParams.enableVsync) {
            if (SUCCEEDED(hr)) {
                SDL_LogWarn(SDL_LOG_CATEGORY_APPLICATION,
                            "OS/GPU doesn't support DXGI_FEATURE_PRESENT_ALLOW_TEARING");
            }
            else {
                SDL_LogError(SDL_LOG_CATEGORY_APPLICATION,
                             "IDXGIFactory::CheckFeatureSupport(DXGI_FEATURE_PRESENT_ALLOW_TEARING) failed: %x",
                             hr);
            }
        }
        else {
            SDL_LogWarn(SDL_LOG_CATEGORY_APPLICATION,
                        "D3D11 VRR unavailable: DXGI_FEATURE_PRESENT_ALLOW_TEARING is unavailable (%x)",
                        hr);
        }
    }

    if (!m_DecoderParams.enableVrr) {
        return;
    }

    m_VrrPresentReadyAvailable = initializeVrrPresentReadyFence();
    if (!m_VrrPresentReadyAvailable) {
        SDL_LogWarn(SDL_LOG_CATEGORY_APPLICATION,
                    "D3D11 VRR unavailable: GPU present-ready fencing is unavailable");
    }
}

void D3D11VARenderer::refreshVrrDisplayState()
{
    if (!m_DecoderParams.enableVrr) {
        return;
    }

    DXGI_SWAP_CHAIN_DESC1 swapChainDesc = {};
    HRESULT swapChainDescResult = E_POINTER;
    if (m_SwapChain != nullptr) {
        swapChainDescResult = m_SwapChain->GetDesc1(&swapChainDesc);
    }
    const bool gotSwapChainDesc = swapChainDescResult == S_OK;

    BOOL fullscreenExclusive = FALSE;
    HRESULT fullscreenStateResult = E_POINTER;
    if (m_SwapChain != nullptr) {
        fullscreenStateResult = m_SwapChain->GetFullscreenState(
            &fullscreenExclusive, nullptr);
    }
    const uint32_t windowFlags = m_DecoderParams.window != nullptr ?
        SDL_GetWindowFlags(m_DecoderParams.window) : 0;
    m_VrrBorderlessFlipModel = gotSwapChainDesc &&
        fullscreenStateResult == S_OK &&
        fullscreenExclusive == FALSE &&
        (windowFlags & SDL_WINDOW_FULLSCREEN_DESKTOP) ==
            SDL_WINDOW_FULLSCREEN_DESKTOP &&
        (swapChainDesc.SwapEffect == DXGI_SWAP_EFFECT_FLIP_DISCARD ||
         swapChainDesc.SwapEffect == DXGI_SWAP_EFFECT_FLIP_SEQUENTIAL);
    m_VrrSwapChainAllowsTearing = gotSwapChainDesc &&
        (swapChainDesc.Flags & DXGI_SWAP_CHAIN_FLAG_ALLOW_TEARING) != 0;

    VrrFallbackReason previousReason = m_VrrFallbackReason;
    m_VrrFallbackReason = evaluateVrrEligibility();

    if (m_VrrFallbackReason != previousReason) {
        SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION,
                    "D3D11 VRR: %s",
                    vrrFallbackReasonName(m_VrrFallbackReason));
    }
}

VrrFallbackReason D3D11VARenderer::evaluateVrrEligibility()
{
    if (!m_DecoderParams.enableVsync) {
        return VrrFallbackReason::IneffectiveVsync;
    }
    // The renderer may fall back to an adapter other than the one that owns
    // the SDL output, which cannot drive that output's flip queue.
    if (!m_VrrBorderlessFlipModel ||
            m_RenderAdapterIndex < 0 ||
            m_RenderAdapterIndex != m_AdapterIndex) {
        return VrrFallbackReason::UnsupportedRenderer;
    }
    if (m_DecoderParams.vrrDisplayRefreshHz <= 0) {
        return VrrFallbackReason::InvalidRefresh;
    }
    if (!isRenderThreadSupported()) {
        return VrrFallbackReason::MainThreadRenderer;
    }
    if (!m_VrrPresentReadyAvailable) {
        return VrrFallbackReason::AdaptivePresentationUnavailable;
    }
    if (!m_VrrSwapChainAllowsTearing) {
        return VrrFallbackReason::InitializationFailed;
    }

    return VrrFallbackReason::NoFallback;
}

void D3D11VARenderer::releasePreparedVrrFrame(
    bool preserveBlackTransition)
{
    // Present() unbinds the render target itself.  A cancellation does not,
    // so explicitly remove the context's reference to the back buffer before
    // a resize/device reset can tear it down.
    if ((m_VrrFramePrepared || m_VrrContextLocked) &&
            m_RenderDeviceContext != nullptr) {
        m_RenderDeviceContext->OMSetRenderTargets(0, nullptr, nullptr);
    }

    m_VrrFramePrepared = false;
    if (!preserveBlackTransition) {
        m_BlackFrameInsertionBlackPresented = false;
    }

    if (m_VrrContextLocked) {
        unlockContext(this);
        m_VrrContextLocked = false;
    }
}

void D3D11VARenderer::queueRenderDeviceReset()
{
    SDL_Event event = {};
    event.type = SDL_RENDER_DEVICE_RESET;
    SDL_PushEvent(&event);
}

bool D3D11VARenderer::prepareFrameForPresent(AVFrame* frame)
{
    if (frame == nullptr || m_RenderDeviceContext == nullptr ||
            m_RenderTargetView == nullptr || m_SwapChain == nullptr) {
        return false;
    }

    // Clear the back buffer.
    const float clearColor[4] = {0.0f, 0.0f, 0.0f, 1.0f};
    m_RenderDeviceContext->ClearRenderTargetView(m_RenderTargetView.Get(), clearColor);

    // Bind the back buffer. This needs to be done each time because Present()
    // unbinds the render target view.
    m_RenderDeviceContext->OMSetRenderTargets(1, m_RenderTargetView.GetAddressOf(), nullptr);

    if (m_BlackFrameInsertionActive) {
        if (!updateBfiConstants(frame)) {
            return false;
        }

        ID3D11Buffer* brightnessConstants =
            m_BlackFrameInsertionDropRecovery ?
                m_BlackFrameInsertionRecoveryConstants.Get() :
                m_BlackFrameInsertionBrightConstants.Get();
        if (brightnessConstants == nullptr) {
            return false;
        }
        m_RenderDeviceContext->PSSetConstantBuffers(
            1, 1, &brightnessConstants);
    }

    // Render the video and overlays.  This is the complete preparation phase
    // shared by the legacy and VRR paths; only the final Present is split out.
    renderVideo(frame);
    for (int i = 0; i < Overlay::OverlayMax; i++) {
        renderOverlay((Overlay::OverlayType)i);
    }

    const AVColorTransferCharacteristic outputTrc =
        m_BlackFrameInsertionActive ? AVCOL_TRC_SMPTE2084 : frame->color_trc;
    if (outputTrc != m_LastColorTrc) {
        HRESULT hr;
        if (outputTrc == AVCOL_TRC_SMPTE2084) {
            hr = m_SwapChain->SetColorSpace1(DXGI_COLOR_SPACE_RGB_FULL_G2084_NONE_P2020);
            if (FAILED(hr)) {
                SDL_LogError(SDL_LOG_CATEGORY_APPLICATION,
                             "IDXGISwapChain::SetColorSpace1(DXGI_COLOR_SPACE_RGB_FULL_G2084_NONE_P2020) failed: %x",
                             hr);
            }
        }
        else {
            hr = m_SwapChain->SetColorSpace1(DXGI_COLOR_SPACE_RGB_FULL_G22_NONE_P709);
            if (FAILED(hr)) {
                SDL_LogError(SDL_LOG_CATEGORY_APPLICATION,
                             "IDXGISwapChain::SetColorSpace1(DXGI_COLOR_SPACE_RGB_FULL_G22_NONE_P709) failed: %x",
                             hr);
            }
        }

        m_LastColorTrc = outputTrc;
    }

    if (m_BlackFrameInsertionActive) {
        if (m_RenderTargetTexture == nullptr ||
                m_BlackFrameInsertionVideoTexture == nullptr) {
            return false;
        }
        m_RenderDeviceContext->CopyResource(
            m_BlackFrameInsertionVideoTexture.Get(),
            m_RenderTargetTexture.Get());

        // Publish restore eligibility only after the complete copy has been
        // submitted. The immediate context orders the later restore copy
        // after this one, so a valid cache always yields the full surface.
        D3D11_TEXTURE2D_DESC cacheDesc = {};
        m_BlackFrameInsertionVideoTexture->GetDesc(&cacheDesc);
        m_BlackFrameInsertionCacheWidth = cacheDesc.Width;
        m_BlackFrameInsertionCacheHeight = cacheDesc.Height;
        m_BlackFrameInsertionCacheValid = true;
    }

    return true;
}

bool D3D11VARenderer::initializeVrrPresentReadyFence()
{
    HRESULT hr = m_RenderDevice->CreateFence(
        0, D3D11_FENCE_FLAG_NONE,
        IID_PPV_ARGS(&m_VrrPresentReadyFence));
    if (SUCCEEDED(hr)) {
        m_VrrPresentReadyFenceEvent = CreateEventW(
            nullptr, FALSE, FALSE, nullptr);
    }

    if (FAILED(hr) || m_VrrPresentReadyFenceEvent == nullptr) {
        SDL_LogWarn(SDL_LOG_CATEGORY_APPLICATION,
                    "D3D11 VRR could not create the present-ready fence: %x",
                    hr);
        m_VrrPresentReadyFence.Reset();
        if (m_VrrPresentReadyFenceEvent != nullptr) {
            CloseHandle(m_VrrPresentReadyFenceEvent);
            m_VrrPresentReadyFenceEvent = nullptr;
        }
        return false;
    }

    return true;
}

bool D3D11VARenderer::waitForVrrPresentReady()
{
    if (!m_VrrPresentReadyAvailable ||
            m_VrrPresentReadyFence == nullptr ||
            m_VrrPresentReadyFenceEvent == nullptr) {
        return false;
    }

    // A tearing Present does not become scanout-visible until all GPU work
    // targeting its back buffer is complete (roughly 2.4 ms was measured
    // between the CPU Present call and display without this fence). Finish
    // the frame here so the worker's later target hold operates on an actual
    // flip-ready boundary.
    const UINT64 fenceValue = ++m_VrrPresentReadyFenceValue;
    HRESULT hr = m_RenderDeviceContext->Signal(
        m_VrrPresentReadyFence.Get(), fenceValue);
    if (FAILED(hr)) {
        SDL_LogError(SDL_LOG_CATEGORY_APPLICATION,
                     "D3D11 VRR present-ready Signal() failed: %x", hr);
        m_VrrPresentReadyAvailable = false;
        return false;
    }

    m_RenderDeviceContext->Flush();
    hr = m_VrrPresentReadyFence->SetEventOnCompletion(
        fenceValue, m_VrrPresentReadyFenceEvent);
    if (FAILED(hr)) {
        SDL_LogError(SDL_LOG_CATEGORY_APPLICATION,
                     "D3D11 VRR present-ready SetEventOnCompletion() failed: %x",
                     hr);
        m_VrrPresentReadyAvailable = false;
        return false;
    }

    // A shared decode/render device uses this mutex in FFmpeg's device
    // callbacks. Never wait for the GPU while retaining it: doing so stalls
    // decode behind the render fence.
    const bool releaseSharedContext =
        m_DecodeDevice == m_RenderDevice && m_VrrContextLocked;
    if (releaseSharedContext) {
        unlockContext(this);
        m_VrrContextLocked = false;
    }

    const DWORD waitResult = WaitForSingleObject(
        m_VrrPresentReadyFenceEvent, 50);

    if (releaseSharedContext) {
        lockContext(this);
        m_VrrContextLocked = true;
    }

    if (waitResult != WAIT_OBJECT_0) {
        SDL_LogError(SDL_LOG_CATEGORY_APPLICATION,
                     "D3D11 VRR present-ready fence wait failed or timed out: %lu",
                     static_cast<unsigned long>(waitResult));
        m_VrrPresentReadyAvailable = false;
        return false;
    }

    return true;
}

HRESULT D3D11VARenderer::presentPreparedFrame(UINT flags)
{
    if (m_SwapChain == nullptr) {
        return E_FAIL;
    }

    return m_SwapChain->Present(0, flags);
}

bool D3D11VARenderer::initializeBlackFrameInsertion()
{
    // The D3D resources may be recreated when the display changes. Force the
    // first frame after reinitialization to repopulate both dynamic buffers.
    m_LastBfiColorSpace = -1;
    m_BlackFrameInsertionLeadTimeUs = 0;
    m_BlackFrameInsertionDropRecovery = false;
    m_BlackFrameInsertionForceTearing = false;
    if (!m_DecoderParams.enableBlackFrameInsertion) {
        return true;
    }

    int displayRefreshHz = 0;
    if (!StreamUtils::tryGetDisplayRefreshRate(
            m_DecoderParams.window, displayRefreshHz)) {
        SDL_LogWarn(SDL_LOG_CATEGORY_APPLICATION,
                    "Black frame insertion disabled: display refresh rate is unavailable");
        return true;
    }

    if (m_DecoderParams.frameRate <= 0) {
        SDL_LogWarn(SDL_LOG_CATEGORY_APPLICATION,
                    "Black frame insertion disabled: stream frame rate is invalid");
        return true;
    }

    const int requiredRefreshHz = m_DecoderParams.frameRate * 2;
    const bool refreshRateSupported = !m_DecoderParams.enableVrr &&
        displayRefreshHz == requiredRefreshHz;
    if (!refreshRateSupported) {
        SDL_LogWarn(SDL_LOG_CATEGORY_APPLICATION,
                    "Black frame insertion disabled: %d FPS stream requires fixed %d Hz V-Sync",
                    m_DecoderParams.frameRate, requiredRefreshHz);
        return true;
    }

    UINT colorSpaceSupport = 0;
    HRESULT hr = m_SwapChain->CheckColorSpaceSupport(
        DXGI_COLOR_SPACE_RGB_FULL_G2084_NONE_P2020, &colorSpaceSupport);
    if (FAILED(hr) || !(colorSpaceSupport & DXGI_SWAP_CHAIN_COLOR_SPACE_SUPPORT_FLAG_PRESENT)) {
        SDL_LogWarn(SDL_LOG_CATEGORY_APPLICATION,
                    "Black frame insertion disabled: the current display does not support HDR10 presentation");
        return true;
    }

    m_BlackFrameInsertionActive = true;
    // Keep black and boosted video on screen for equal durations. Using one
    // native display period made the duty cycle depend on the panel ceiling
    // (for example, only 20% black at 60 FPS on a 300 Hz panel), so a normal
    // 600-nit BFI frame and a 300-nit recovery frame had different average
    // luminance and visibly pulsed.
    m_BlackFrameInsertionLeadTimeUs =
        1000000ULL /
        (static_cast<uint64_t>(m_DecoderParams.frameRate) * 2ULL);
    m_BlackFrameInsertionForceTearing =
        m_DecoderParams.enableVrr &&
        qEnvironmentVariableIntValue(
            "MOONLIGHT_BFI_FORCE_TEARING") != 0;
    SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION,
                "Black frame insertion enabled: SDR reference white is rendered at 600 nits with a 50%% duty cycle (%llu us black interval)%s",
                static_cast<unsigned long long>(
                    m_BlackFrameInsertionLeadTimeUs),
                " using a frame-independent fixed-VSync carrier");
    if (m_BlackFrameInsertionForceTearing) {
        SDL_LogWarn(
            SDL_LOG_CATEGORY_APPLICATION,
            "BFI tearing A/B enabled by MOONLIGHT_BFI_FORCE_TEARING: "
            "black, video, and recovery Presents will use "
            "DXGI_PRESENT_ALLOW_TEARING");
    }

    return true;
}

bool D3D11VARenderer::restoreBlackFrameInsertionVideo()
{
    if (m_RenderDeviceContext == nullptr ||
            m_RenderTargetTexture == nullptr ||
            m_BlackFrameInsertionVideoTexture == nullptr) {
        return false;
    }

    // Cache existence is not restore eligibility. Without a published
    // complete copy for the current target, the cache may hold uninitialized
    // or stale data (for example between a resize and the next prepared
    // frame), so fail closed and let the caller's black/reprepare path run.
    bool sizeMatched = true;
    if (m_BlackFrameInsertionCacheValid) {
        D3D11_TEXTURE2D_DESC targetDesc = {};
        m_RenderTargetTexture->GetDesc(&targetDesc);
        sizeMatched = targetDesc.Width == m_BlackFrameInsertionCacheWidth &&
            targetDesc.Height == m_BlackFrameInsertionCacheHeight;
    }
    if (!m_BlackFrameInsertionCacheValid || !sizeMatched) {
        m_BlackFrameInsertionCacheValid = false;
        m_BlackFrameInsertionRestoreRejects++;
        // Power-of-two backoff keeps this diagnostic bounded if rejection
        // recurs at the carrier rate.
        if ((m_BlackFrameInsertionRestoreRejects &
                (m_BlackFrameInsertionRestoreRejects - 1)) == 0) {
            SDL_LogWarn(SDL_LOG_CATEGORY_APPLICATION,
                        "BFI video restore rejected: cache is not a complete copy of the current render target (%s, count=%u)",
                        sizeMatched ? "not published" : "dimension mismatch",
                        m_BlackFrameInsertionRestoreRejects);
        }
        return false;
    }

    m_RenderDeviceContext->CopyResource(
        m_RenderTargetTexture.Get(),
        m_BlackFrameInsertionVideoTexture.Get());
    return true;
}

HRESULT D3D11VARenderer::presentBlackFrame(
    UINT syncInterval, UINT flags)
{
    if (m_RenderDeviceContext == nullptr || m_RenderTargetView == nullptr ||
            m_SwapChain == nullptr) {
        return E_FAIL;
    }

    const float black[4] = {0.0f, 0.0f, 0.0f, 1.0f};
    m_RenderDeviceContext->ClearRenderTargetView(m_RenderTargetView.Get(), black);
    return m_SwapChain->Present(syncInterval, flags);
}

UINT D3D11VARenderer::bfiVrrPresentFlags(
    const VrrPresentRequest&) const
{
    // Every BFI present participates in the black/boosted luminance cycle. A
    // torn present splits the screen between two luminance states at the tear
    // line, which reads as a large dark band, so BFI ignores the controller's
    // latched/immediate preference and always presents non-tearing. The
    // MOONLIGHT_BFI_FORCE_TEARING A/B remains the explicit override.
    return m_BlackFrameInsertionForceTearing ?
        DXGI_PRESENT_ALLOW_TEARING : 0;
}

void D3D11VARenderer::populateVrrPresentFeedback(
    VrrPresentFeedback& feedback, UINT presentFlags)
{
    feedback.presentModeValid = true;
    feedback.tearingAllowed =
        (presentFlags & DXGI_PRESENT_ALLOW_TEARING) != 0;

    UINT lastPresentCount = 0;
    if (SUCCEEDED(m_SwapChain->GetLastPresentCount(
            &lastPresentCount))) {
        feedback.submissionIdValid = true;
        feedback.submissionId = lastPresentCount;
    }

    DXGI_FRAME_STATISTICS frameStats = {};
    if (SUCCEEDED(m_SwapChain->GetFrameStatistics(&frameStats))) {
        feedback.latchSampleValid = true;
        feedback.latchSubmissionId = frameStats.PresentCount;
        feedback.latchPresentRefreshSequence =
            frameStats.PresentRefreshCount;
        feedback.latchRefreshSequence = frameStats.SyncRefreshCount;
        feedback.latchTimeValid = translateQpcToPacingTime(
            frameStats.SyncQPCTime, feedback.latchTimeUs);
    }
}

IVrrFramePresenter* D3D11VARenderer::getVrrFramePresenter()
{
    return this;
}

VrrFallbackReason D3D11VARenderer::checkSupport() const
{
    if (m_VrrFallbackReason != VrrFallbackReason::NoFallback) {
        return m_VrrFallbackReason;
    }

    return m_DecoderParams.enableVrr && m_SwapChain != nullptr &&
        m_VrrSwapChainAllowsTearing ? VrrFallbackReason::NoFallback :
        VrrFallbackReason::InitializationFailed;
}

VrrPrepareResult D3D11VARenderer::prepareFrame(AVFrame* frame)
{
    VrrPrepareResult result;
    if (m_VrrSuspended || frame == nullptr) {
        return result;
    }

    // The contract guarantees one worker, but make an accidental second
    // preparation recoverable instead of leaking a retained context lock.
    if (m_VrrFramePrepared) {
        SDL_LogWarn(SDL_LOG_CATEGORY_APPLICATION,
                    "D3D11 VRR discarded an unpresented prepared frame");
        result.feedback = cancelFrame();
        return result;
    }

    // Hold the FFmpeg/D3D context lock from rendering preparation through
    // final Present.  renderVideo() recognizes this retained lock for the
    // separate-device fence calls, so it never recursively locks the SDL
    // mutex while this frame is in flight.
    lockContext(this);
    m_VrrContextLocked = true;

    // Display-state changes share this lock with preparation through Present.
    // Check eligibility only after taking it so a UI callback cannot replace
    // swapchain state concurrently.
    if (checkSupport() != VrrFallbackReason::NoFallback) {
        releasePreparedVrrFrame(
            m_BlackFrameInsertionBlackPresented);
        return result;
    }

    if (!prepareFrameForPresent(frame)) {
        releasePreparedVrrFrame(
            m_BlackFrameInsertionBlackPresented);
        return result;
    }

    if (!waitForVrrPresentReady()) {
        m_VrrFallbackReason = VrrFallbackReason::AdaptivePresentationUnavailable;
        result.feedback.cancelled = true;
        releasePreparedVrrFrame(
            m_BlackFrameInsertionBlackPresented);
        queueRenderDeviceReset();
        return result;
    }

    // The shared-device fence wait temporarily releases the renderer lock.
    // Revalidate state after reacquiring it before publishing this frame as
    // prepared for the worker's target wait.
    if (m_VrrSuspended || checkSupport() != VrrFallbackReason::NoFallback) {
        result.feedback.cancelled = true;
        releasePreparedVrrFrame(
            m_BlackFrameInsertionBlackPresented);
        return result;
    }

    HRESULT deviceReason = m_RenderDevice->GetDeviceRemovedReason();
    if (FAILED(deviceReason)) {
        SDL_LogError(SDL_LOG_CATEGORY_APPLICATION,
                     "D3D11 VRR preparation detected device loss: %x",
                     deviceReason);
        result.feedback.cancelled = true;
        releasePreparedVrrFrame(
            m_BlackFrameInsertionBlackPresented);
        queueRenderDeviceReset();
        return result;
    }

    m_VrrFramePrepared = true;
    result.prepared = true;
    return result;
}

uint64_t D3D11VARenderer::prePresentLeadTimeUs() const
{
    return m_BlackFrameInsertionActive ?
        m_BlackFrameInsertionLeadTimeUs : 0;
}

VrrPresentFeedback D3D11VARenderer::presentPreFrame(
    const VrrPresentRequest& request)
{
    VrrPresentFeedback feedback;
    if (!m_VrrFramePrepared || m_VrrSuspended ||
            !m_BlackFrameInsertionActive) {
        feedback.cancelled = true;
        return feedback;
    }

    const UINT presentFlags = bfiVrrPresentFlags(request);
    const uint64_t submissionTimeUs = LiGetMicroseconds();
    const HRESULT hr = presentBlackFrame(0, presentFlags);
    if (hr != S_OK || !restoreBlackFrameInsertionVideo()) {
        SDL_LogError(SDL_LOG_CATEGORY_APPLICATION,
                     "D3D11 VRR BFI black transition failed: %x", hr);
        releasePreparedVrrFrame();
        queueRenderDeviceReset();
        feedback.cancelled = true;
        return feedback;
    }

    m_BlackFrameInsertionBlackPresented = true;
    feedback.presented = true;
    feedback.submissionTimeValid = true;
    feedback.submissionTimeUs = submissionTimeUs;
    populateVrrPresentFeedback(feedback, presentFlags);
    return feedback;
}

VrrPresentFeedback D3D11VARenderer::presentIdlePreFrame(
    const VrrPresentRequest& request)
{
    VrrPresentFeedback feedback;
    if (!m_BlackFrameInsertionActive || m_VrrSuspended ||
            m_VrrFramePrepared || m_VrrContextLocked ||
            m_BlackFrameInsertionBlackPresented) {
        feedback.cancelled = true;
        return feedback;
    }

    // Begin the black phase without waiting for preparation of the next source
    // frame. Keep the cached lit image in the new back buffer so a later
    // preparation failure or interruption can restore it immediately.
    lockContext(this);
    if (!m_BlackFrameInsertionActive || m_VrrSuspended ||
            m_RenderDeviceContext == nullptr ||
            m_RenderTargetView == nullptr ||
            m_RenderTargetTexture == nullptr ||
            m_BlackFrameInsertionVideoTexture == nullptr ||
            m_SwapChain == nullptr) {
        unlockContext(this);
        feedback.cancelled = true;
        return feedback;
    }

    const UINT presentFlags = bfiVrrPresentFlags(request);
    const uint64_t submissionTimeUs = LiGetMicroseconds();
    const HRESULT hr = presentBlackFrame(0, presentFlags);
    const bool restored = hr == S_OK && restoreBlackFrameInsertionVideo();
    if (restored) {
        m_BlackFrameInsertionBlackPresented = true;
    }
    unlockContext(this);

    if (FAILED(hr)) {
        SDL_LogError(SDL_LOG_CATEGORY_APPLICATION,
                     "D3D11 VRR BFI idle black transition failed: %x", hr);
        queueRenderDeviceReset();
        feedback.cancelled = true;
        return feedback;
    }
    if (hr != S_OK || !restored) {
        feedback.cancelled = true;
        return feedback;
    }

    feedback.presented = true;
    feedback.submissionTimeValid = true;
    feedback.submissionTimeUs = submissionTimeUs;
    populateVrrPresentFeedback(feedback, presentFlags);
    return feedback;
}

VrrPresentFeedback D3D11VARenderer::presentIdleFrameRepeat(
    const VrrPresentRequest& request)
{
    return presentPairRepeatVideo(request);
}

VrrPresentFeedback D3D11VARenderer::presentPairRepeatBlack(
    const VrrPresentRequest& request)
{
    VrrPresentFeedback feedback;
    if (!m_BlackFrameInsertionActive || m_VrrSuspended ||
            m_VrrFramePrepared || m_VrrContextLocked) {
        return feedback;
    }

    // No source frame is prepared; serialize this cached-image pass
    // explicitly against FFmpeg and window changes.
    lockContext(this);
    if (!m_BlackFrameInsertionActive || m_VrrSuspended ||
            m_RenderDeviceContext == nullptr ||
            m_RenderTargetView == nullptr ||
            m_RenderTargetTexture == nullptr ||
            m_BlackFrameInsertionVideoTexture == nullptr ||
            m_SwapChain == nullptr) {
        unlockContext(this);
        return feedback;
    }

    const UINT presentFlags = bfiVrrPresentFlags(request);
    const uint64_t submissionTimeUs = LiGetMicroseconds();
    const HRESULT hr = presentBlackFrame(0, presentFlags);
    const bool restored = hr == S_OK && restoreBlackFrameInsertionVideo();
    unlockContext(this);

    if (FAILED(hr)) {
        SDL_LogError(SDL_LOG_CATEGORY_APPLICATION,
                     "D3D11 VRR BFI pair-repeat black failed: %x", hr);
        queueRenderDeviceReset();
        feedback.cancelled = true;
        return feedback;
    }
    if (hr != S_OK || !restored) {
        feedback.cancelled = true;
        return feedback;
    }

    feedback.presented = true;
    feedback.submissionTimeValid = true;
    feedback.submissionTimeUs = submissionTimeUs;
    m_BlackFrameInsertionBlackPresented = true;
    populateVrrPresentFeedback(feedback, presentFlags);
    return feedback;
}

VrrPresentFeedback D3D11VARenderer::presentPairRepeatVideo(
    const VrrPresentRequest& request)
{
    VrrPresentFeedback feedback;
    if (!m_BlackFrameInsertionActive || m_VrrSuspended ||
            m_VrrFramePrepared || m_VrrContextLocked) {
        return feedback;
    }

    lockContext(this);
    if (!m_BlackFrameInsertionActive || m_VrrSuspended ||
            m_RenderDeviceContext == nullptr ||
            m_RenderTargetTexture == nullptr ||
            m_BlackFrameInsertionVideoTexture == nullptr ||
            m_SwapChain == nullptr ||
            !restoreBlackFrameInsertionVideo()) {
        unlockContext(this);
        return feedback;
    }

    const UINT presentFlags = bfiVrrPresentFlags(request);
    const uint64_t submissionTimeUs = LiGetMicroseconds();
    const HRESULT hr = presentPreparedFrame(presentFlags);
    unlockContext(this);

    if (FAILED(hr)) {
        SDL_LogError(SDL_LOG_CATEGORY_APPLICATION,
                     "D3D11 VRR BFI pair-repeat video failed: %x", hr);
        queueRenderDeviceReset();
        feedback.cancelled = true;
        return feedback;
    }
    if (hr != S_OK) {
        feedback.cancelled = true;
        return feedback;
    }

    feedback.presented = true;
    feedback.submissionTimeValid = true;
    feedback.submissionTimeUs = submissionTimeUs;
    m_BlackFrameInsertionBlackPresented = false;
    populateVrrPresentFeedback(feedback, presentFlags);
    return feedback;
}

VrrPresentFeedback D3D11VARenderer::presentAdaptive(
    const VrrPresentRequest& request)
{
    VrrPresentFeedback feedback;

    if (!m_VrrFramePrepared || m_VrrSuspended) {
        return cancelFrame();
    }
    if (m_BlackFrameInsertionActive &&
            !m_BlackFrameInsertionDropRecovery &&
            !m_BlackFrameInsertionBlackPresented) {
        SDL_LogError(SDL_LOG_CATEGORY_APPLICATION,
                     "D3D11 VRR BFI video present had no black transition");
        return cancelFrame();
    }

    // A latched near-refresh request omits ALLOW_TEARING, selecting DXGI's
    // non-tearing path. The flag is a per-present choice on this swapchain,
    // so no swapchain recreation is involved.
    const UINT presentFlags = m_BlackFrameInsertionActive ?
        bfiVrrPresentFlags(request) :
        (request.latchedPresentation ?
            0 : DXGI_PRESENT_ALLOW_TEARING);

    // The worker anchors its display-spacing floor at this instant rather
    // than at its own call boundary, so capture it immediately before Present.
    const uint64_t submissionTimeUs = LiGetMicroseconds();
    HRESULT hr = presentPreparedFrame(presentFlags);

    if (FAILED(hr)) {
        releasePreparedVrrFrame();
        SDL_LogError(SDL_LOG_CATEGORY_APPLICATION,
                     "IDXGISwapChain::Present() failed on D3D11 VRR path: %x",
                     hr);
        queueRenderDeviceReset();
        feedback.cancelled = true;
        return feedback;
    }
    if (hr != S_OK) {
        // In particular, DXGI_STATUS_OCCLUDED is a successful HRESULT but
        // does not mean that the image reached a monitor.
        releasePreparedVrrFrame();
        SDL_LogWarn(SDL_LOG_CATEGORY_APPLICATION,
                     "IDXGISwapChain::Present() returned non-display status on D3D11 VRR path: %x",
                     hr);
        feedback.cancelled = true;
        return feedback;
    }

    feedback.presented = true;
    feedback.submissionTimeValid = true;
    feedback.submissionTimeUs = submissionTimeUs;
    if (m_BlackFrameInsertionActive) {
        populateVrrPresentFeedback(feedback, presentFlags);
    }
    releasePreparedVrrFrame();
    return feedback;
}

VrrPresentFeedback D3D11VARenderer::cancelFrame()
{
    VrrPresentFeedback feedback;

    // Once the black transition has reached DXGI, abandoning the prepared
    // frame without another Present can leave black on screen until the next
    // decoded frame. End an interrupted pair on the cached lit image. A
    // minimized window is not visible and may only return OCCLUDED, so defer
    // to normal restore/recreation in that state.
    if (m_BlackFrameInsertionBlackPresented && !m_VrrSuspended) {
        // An idle-started black transition can be pending before preparation
        // acquires the retained renderer lock. Serialize the cached-image
        // restore explicitly in that case.
        if (!m_VrrContextLocked) {
            lockContext(this);
            m_VrrContextLocked = true;
        }

        // A cancellation restore is still one half of a BFI luminance
        // transition. Keep it on the same non-tearing path as ordinary black,
        // video, and recovery presents so an interruption cannot introduce a
        // full-width bright/dark tear band. The explicit tearing A/B remains
        // available through bfiVrrPresentFlags().
        const UINT presentFlags =
            bfiVrrPresentFlags(VrrPresentRequest {});
        const bool restored = restoreBlackFrameInsertionVideo();
        const uint64_t submissionTimeUs = LiGetMicroseconds();
        const HRESULT hr = restored ?
            presentPreparedFrame(presentFlags) : E_FAIL;
        if (hr == S_OK) {
            feedback.presented = true;
            feedback.submissionTimeValid = true;
            feedback.submissionTimeUs = submissionTimeUs;
            populateVrrPresentFeedback(feedback, presentFlags);
        }
        else {
            SDL_LogWarn(SDL_LOG_CATEGORY_APPLICATION,
                        "D3D11 VRR BFI could not restore video while cancelling a black transition: %x",
                        hr);
        }
    }

    releasePreparedVrrFrame();
    feedback.cancelled = true;
    return feedback;
}

bool D3D11VARenderer::restoreFixedPresentation(VrrFallbackReason reason)
{
    // This is called synchronously only if the pacing worker could not start,
    // before it has prepared a frame.  ALLOW_TEARING is a swapchain capability,
    // not a requirement for every Present, so the existing swapchain safely
    // supports the legacy fixed path with Present(0, 0).  Do not recreate it.
    cancelFrame();
    m_VrrSuspended = false;
    m_DecoderParams.enableVrr = false;
    m_BlackFrameInsertionDropRecovery = false;
    m_BlackFrameInsertionActive = false;
    if (!initializeBlackFrameInsertion()) {
        return false;
    }
    m_VrrFallbackReason = reason == VrrFallbackReason::NoFallback ?
        VrrFallbackReason::InitializationFailed : reason;
    return true;
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

        QByteArray bfiOverlayPixelShaderBytecode = Path::readDataFile("d3d11_overlay_bfi_pixel.fxc");
        hr = m_RenderDevice->CreatePixelShader(bfiOverlayPixelShaderBytecode.constData(), bfiOverlayPixelShaderBytecode.length(), nullptr, &m_BfiOverlayPixelShader);
        if (FAILED(hr)) {
            SDL_LogError(SDL_LOG_CATEGORY_APPLICATION,
                         "ID3D11Device::CreatePixelShader() failed: %x",
                         hr);
            return false;
        }

        QByteArray bfiDimPixelShaderBytecode =
            Path::readDataFile("d3d11_bfi_dim_pixel.fxc");
        hr = m_RenderDevice->CreatePixelShader(
            bfiDimPixelShaderBytecode.constData(),
            bfiDimPixelShaderBytecode.length(), nullptr,
            &m_BlackFrameInsertionDimPixelShader);
        if (FAILED(hr)) {
            SDL_LogError(SDL_LOG_CATEGORY_APPLICATION,
                         "Unable to create BFI idle-dimming pixel shader: %x",
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

    // BFI brightness is selected without changing shaders or adding a pass.
    {
        const VERTEX fullscreenVertices[] =
        {
            {-1.0f, -1.0f, 0.0f, 1.0f},
            {-1.0f,  1.0f, 0.0f, 0.0f},
            { 1.0f, -1.0f, 1.0f, 1.0f},
            { 1.0f,  1.0f, 1.0f, 0.0f},
        };
        D3D11_BUFFER_DESC vertexBufferDesc = {};
        vertexBufferDesc.ByteWidth = sizeof(fullscreenVertices);
        vertexBufferDesc.Usage = D3D11_USAGE_IMMUTABLE;
        vertexBufferDesc.BindFlags = D3D11_BIND_VERTEX_BUFFER;
        vertexBufferDesc.StructureByteStride = sizeof(VERTEX);
        D3D11_SUBRESOURCE_DATA vertexBufferData = {};
        vertexBufferData.pSysMem = fullscreenVertices;
        hr = m_RenderDevice->CreateBuffer(
            &vertexBufferDesc, &vertexBufferData,
            &m_BlackFrameInsertionFullscreenVertexBuffer);
        if (FAILED(hr)) {
            SDL_LogError(SDL_LOG_CATEGORY_APPLICATION,
                         "Unable to create BFI fullscreen vertex buffer: %x",
                         hr);
            return false;
        }

        D3D11_BUFFER_DESC constantBufferDesc = {};
        constantBufferDesc.ByteWidth = sizeof(BFI_CONST_BUF);
        constantBufferDesc.Usage = D3D11_USAGE_DYNAMIC;
        constantBufferDesc.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
        constantBufferDesc.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;

        hr = m_RenderDevice->CreateBuffer(
            &constantBufferDesc, nullptr,
            &m_BlackFrameInsertionBrightConstants);
        if (FAILED(hr)) {
            SDL_LogError(SDL_LOG_CATEGORY_APPLICATION,
                         "Unable to create BFI brightness constants: %x",
                         hr);
            return false;
        }

        hr = m_RenderDevice->CreateBuffer(
            &constantBufferDesc, nullptr,
            &m_BlackFrameInsertionRecoveryConstants);
        if (FAILED(hr)) {
            SDL_LogError(SDL_LOG_CATEGORY_APPLICATION,
                         "Unable to create BFI recovery constants: %x",
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

    // A freshly created BFI cache texture has undefined contents. Publish it
    // as invalid; only a complete copy in prepareFrameForPresent() may make
    // it eligible for restoration.
    m_BlackFrameInsertionCacheValid = false;

    // Create our render target view
    {
        hr = m_SwapChain->GetBuffer(
            0, IID_PPV_ARGS(&m_RenderTargetTexture));
        if (FAILED(hr)) {
            SDL_LogError(SDL_LOG_CATEGORY_APPLICATION,
                         "IDXGISwapChain::GetBuffer() failed: %x",
                         hr);
            return false;
        }

        hr = m_RenderDevice->CreateRenderTargetView(
            m_RenderTargetTexture.Get(), nullptr, &m_RenderTargetView);
        if (FAILED(hr)) {
            SDL_LogError(SDL_LOG_CATEGORY_APPLICATION,
                         "ID3D11Device::CreateRenderTargetView() failed: %x",
                         hr);
            return false;
        }

        if (m_BlackFrameInsertionActive) {
            D3D11_TEXTURE2D_DESC textureDesc = {};
            m_RenderTargetTexture->GetDesc(&textureDesc);
            textureDesc.BindFlags = D3D11_BIND_SHADER_RESOURCE;
            textureDesc.CPUAccessFlags = 0;
            textureDesc.MiscFlags = 0;
            textureDesc.Usage = D3D11_USAGE_DEFAULT;
            hr = m_RenderDevice->CreateTexture2D(
                &textureDesc, nullptr,
                &m_BlackFrameInsertionVideoTexture);
            if (FAILED(hr)) {
                SDL_LogError(SDL_LOG_CATEGORY_APPLICATION,
                             "Unable to create BFI video cache texture: %x",
                             hr);
                return false;
            }

            hr = m_RenderDevice->CreateShaderResourceView(
                m_BlackFrameInsertionVideoTexture.Get(), nullptr,
                &m_BlackFrameInsertionVideoTextureSrv);
            if (FAILED(hr)) {
                SDL_LogError(SDL_LOG_CATEGORY_APPLICATION,
                             "Unable to create BFI video cache view: %x",
                             hr);
                return false;
            }
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

    // Create SRVs for the texture
    D3D11_SHADER_RESOURCE_VIEW_DESC srvDesc = {};
    srvDesc.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2D;
    srvDesc.Texture2D.MostDetailedMip = 0;
    srvDesc.Texture2D.MipLevels = 1;
    size_t srvIndex = 0;
    for (DXGI_FORMAT srvFormat : getVideoTextureSRVFormats()) {
        SDL_assert(srvIndex < m_VideoTextureResourceViews[0].size());

        srvDesc.Format = srvFormat;
        hr = m_RenderDevice->CreateShaderResourceView(m_VideoTexture.Get(), &srvDesc, &m_VideoTextureResourceViews[0][srvIndex]);
        if (FAILED(hr)) {
            SDL_LogError(SDL_LOG_CATEGORY_APPLICATION,
                         "ID3D11Device::CreateShaderResourceView() failed: %x",
                         hr);
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

    D3D11_SHADER_RESOURCE_VIEW_DESC srvDesc = {};
    srvDesc.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2DARRAY;
    srvDesc.Texture2DArray.MostDetailedMip = 0;
    srvDesc.Texture2DArray.MipLevels = 1;
    srvDesc.Texture2DArray.ArraySize = 1;

    m_VideoTextureResourceViews.resize(framesContext->initial_pool_size);

    // Create luminance and chrominance SRVs for each texture in the pool
    for (int i = 0; i < framesContext->initial_pool_size; i++) {
        HRESULT hr;

        // Our rendering logic depends on the texture index working to map into our SRV array
        SDL_assert(i == d3d11vaFramesContext->texture_infos[i].index);

        srvDesc.Texture2DArray.FirstArraySlice = d3d11vaFramesContext->texture_infos[i].index;

        size_t srvIndex = 0;
        for (DXGI_FORMAT srvFormat : getVideoTextureSRVFormats()) {
            SDL_assert(srvIndex < m_VideoTextureResourceViews[i].size());

            srvDesc.Format = srvFormat;
            hr = m_RenderDevice->CreateShaderResourceView(m_RenderSharedTextureArray.Get(),
                                                          &srvDesc,
                                                          &m_VideoTextureResourceViews[i][srvIndex]);
            if (FAILED(hr)) {
                SDL_LogError(SDL_LOG_CATEGORY_APPLICATION,
                             "ID3D11Device::CreateShaderResourceView() failed: %x",
                             hr);
                return false;
            }

            srvIndex++;
        }
    }

    return true;
}
