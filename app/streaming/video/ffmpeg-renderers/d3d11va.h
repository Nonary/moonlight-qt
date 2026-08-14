#pragma once

#include "ivrrframepresenter.h"
#include "renderer.h"

#include <d3d11_4.h>
#include <dxgi1_6.h>

extern "C" {
#include <libavutil/hwcontext_d3d11va.h>
}

#include <wrl/client.h>
#include <wrl/wrappers/corewrappers.h>

class D3D11VARenderer : public IFFmpegRenderer, public IVrrFramePresenter
{
public:
    D3D11VARenderer(int decoderSelectionPass);
    virtual ~D3D11VARenderer() override;
    virtual bool initialize(PDECODER_PARAMETERS params) override;
    virtual bool prepareDecoderContext(AVCodecContext* context, AVDictionary**) override;
    virtual bool prepareDecoderContextInGetFormat(AVCodecContext* context, AVPixelFormat pixelFormat) override;
    virtual void renderFrame(AVFrame* frame) override;
    virtual bool needsFrameIndependentRefresh() const override;
    virtual bool renderFrameIndependentRefresh() override;
    virtual void cleanupRenderContext() override;
    virtual IVrrFramePresenter* getVrrFramePresenter() override;

    virtual bool canLatchAdaptivePresent() const override { return true; }
    virtual VrrFallbackReason checkSupport() const override;
    virtual VrrPrepareResult prepareFrame(AVFrame* frame) override;
    virtual uint64_t prePresentLeadTimeUs() const override;
    virtual VrrPresentFeedback presentPreFrame(
        const VrrPresentRequest& request) override;
    virtual VrrPresentFeedback presentIdlePreFrame(
        const VrrPresentRequest& request) override;
    virtual void setFrameDropRecovery(bool enabled) override
        { m_BlackFrameInsertionDropRecovery = enabled; }
    virtual VrrPresentFeedback presentIdleFrameRepeat(
        const VrrPresentRequest& request) override;
    virtual VrrPresentFeedback presentPairRepeatBlack(
        const VrrPresentRequest& request) override;
    virtual VrrPresentFeedback presentPairRepeatVideo(
        const VrrPresentRequest& request) override;
    virtual VrrPresentFeedback presentAdaptive(
        const VrrPresentRequest& request) override;
    virtual VrrPresentFeedback cancelFrame() override;
    virtual void setSuspended(bool suspended) override { m_VrrSuspended = suspended; }
    virtual bool restoreFixedPresentation(VrrFallbackReason reason) override;

    virtual void notifyOverlayUpdated(Overlay::OverlayType) override;
    virtual bool notifyWindowChanged(PWINDOW_STATE_CHANGE_INFO stateInfo) override;
    virtual int getRendererAttributes() override;
    virtual int getDecoderCapabilities() override;
    virtual InitFailureReason getInitFailureReason() override;

    enum PixelShaders {
        GENERIC_YUV_420,
        GENERIC_AYUV,
        GENERIC_Y410,
        BFI_YUV_420,
        BFI_AYUV,
        BFI_Y410,
        _COUNT
    };

private:
    static void lockContext(void* lock_ctx);
    static void unlockContext(void* lock_ctx);

    bool setupRenderingResources();
    std::vector<DXGI_FORMAT> getVideoTextureSRVFormats();
    bool setupFrameRenderingResources(AVHWFramesContext* framesContext);
    bool setupSwapchainDependentResources();
    bool setupVideoTexture(AVHWFramesContext* framesContext); // for !m_BindDecoderOutputTextures
    bool setupTexturePoolViews(AVHWFramesContext* framesContext); // for m_BindDecoderOutputTextures
    bool prepareFrameForPresent(AVFrame* frame);
    bool updateBfiConstants(const AVFrame* frame);
    bool initializeBlackFrameInsertion();
    void releaseBfiFrameLatencyWaitable();
    bool acquireBfiFrameLatencyWaitable();
    bool setBfiSwapchainFrameLatency(UINT latency);
    bool waitForBfiPresentSlot();
    void releaseBfiBackbuffer();
    bool acquireBfiBackbuffer();
    bool copyBfiComposeToBackbuffer();
    void clearBfiBackbuffer();
    bool prepareBfiSwapchain(bool presentVideo);
    HRESULT presentPreparedBfiSwapchain(UINT syncInterval, UINT flags);
    HRESULT presentBfiSwapchain(bool presentVideo, UINT syncInterval, UINT flags);
    bool restoreBlackFrameInsertionVideo();
    HRESULT presentBlackFrame(UINT syncInterval, UINT flags);
    UINT bfiVrrPresentFlags(const VrrPresentRequest& request) const;
    void populateVrrPresentFeedback(VrrPresentFeedback& feedback,
                                    UINT presentFlags);
    bool initializeVrrPresentReadyFence();
    bool waitForVrrPresentReady();
    HRESULT presentPreparedFrame(UINT flags);
    void initializeVrrPresentationState(DXGI_SWAP_CHAIN_DESC1* swapChainDesc);
    void refreshVrrDisplayState();
    VrrFallbackReason evaluateVrrEligibility();
    void releasePreparedVrrFrame(bool preserveBlackTransition = false);
    void queueRenderDeviceReset();
    void renderOverlay(Overlay::OverlayType type);
    bool createOverlayVertexBuffer(Overlay::OverlayType type, int width, int height, Microsoft::WRL::ComPtr<ID3D11Buffer>& newVertexBuffer);
    void bindColorConversion(bool frameChanged, AVFrame* frame);
    void bindVideoVertexBuffer(bool frameChanged, AVFrame* frame);
    void renderVideo(AVFrame* frame);
    bool checkDecoderSupport(IDXGIAdapter* adapter);
    bool createDeviceByAdapterIndex(int adapterIndex, bool* adapterNotFound = nullptr);
    bool setupSharedDevice(IDXGIAdapter1* adapter);
    bool createSharedFencePair(UINT64 initialValue,
                               ID3D11Device5* dev1, ID3D11Device5* dev2,
                               Microsoft::WRL::ComPtr<ID3D11Fence>& dev1Fence,
                               Microsoft::WRL::ComPtr<ID3D11Fence>& dev2Fence);

    int m_DecoderSelectionPass;
    int m_DevicesWithFL11Support;
    int m_DevicesWithCodecSupport;

    enum class SupportedFenceType {
        None,
        NonMonitored,
        Monitored,
    };

    bool m_DebugLayer;
    Microsoft::WRL::ComPtr<IDXGIFactory5> m_Factory;
    // m_AdapterIndex identifies the output selected by SDL.  The renderer
    // may fall back to another adapter for decoding, so retain that index
    // separately for the VRR same-GPU check.
    int m_AdapterIndex;
    int m_RenderAdapterIndex;
    Microsoft::WRL::ComPtr<ID3D11Device5> m_RenderDevice, m_DecodeDevice;
    Microsoft::WRL::ComPtr<ID3D11DeviceContext4> m_RenderDeviceContext, m_DecodeDeviceContext;
    Microsoft::WRL::ComPtr<ID3D11Texture2D> m_RenderSharedTextureArray;
    Microsoft::WRL::ComPtr<IDXGISwapChain4> m_SwapChain;
    Microsoft::WRL::ComPtr<ID3D11Texture2D> m_RenderTargetTexture;
    Microsoft::WRL::ComPtr<ID3D11RenderTargetView> m_RenderTargetView;
    Microsoft::WRL::ComPtr<ID3D11Texture2D> m_BlackFrameInsertionVideoTexture;
    Microsoft::WRL::ComPtr<ID3D11ShaderResourceView> m_BlackFrameInsertionVideoTextureSrv;
    // Transient swapchain backbuffer used only for the current BFI Present.
    // Video is composed offscreen; these are acquired after the waitable slot
    // is free and released before the next carrier half.
    Microsoft::WRL::ComPtr<ID3D11Texture2D> m_BfiBackbufferTexture;
    Microsoft::WRL::ComPtr<ID3D11RenderTargetView> m_BfiBackbufferRtv;
    HANDLE m_BfiFrameLatencyWaitableObject = nullptr;
    Microsoft::WRL::ComPtr<ID3D11BlendState> m_VideoBlendState;
    Microsoft::WRL::ComPtr<ID3D11BlendState> m_OverlayBlendState;

    SupportedFenceType m_FenceType;
    Microsoft::WRL::ComPtr<ID3D11Fence> m_DecodeD2RFence, m_RenderD2RFence;
    UINT64 m_D2RFenceValue;
    Microsoft::WRL::ComPtr<ID3D11Fence> m_DecodeR2DFence, m_RenderR2DFence;
    UINT64 m_R2DFenceValue;
    SDL_mutex* m_ContextLock;
    // Serializes swapchain presentation against resize/display changes without
    // forcing decoder work to wait through blocking fixed-VSync Presents.
    SDL_mutex* m_PresentationLock;
    bool m_BindDecoderOutputTextures;

    DECODER_PARAMETERS m_DecoderParams;
    DXGI_FORMAT m_TextureFormat;
    int m_DisplayWidth;
    int m_DisplayHeight;
    AVColorTransferCharacteristic m_LastColorTrc;
    int m_LastBfiColorSpace = -1;

    bool m_AllowTearing;
    bool m_BlackFrameInsertionActive;
    bool m_BlackFrameInsertionBlackPresented;
    bool m_BlackFrameInsertionDropRecovery;
    bool m_BlackFrameInsertionForceTearing;
    uint64_t m_BlackFrameInsertionLeadTimeUs;
    // The offscreen compose target is only a trustworthy Present source after
    // prepareFrameForPresent() has finished drawing into it. Cleared before
    // any swapchain-dependent resource replacement. Consistency is guaranteed
    // by the existing lock discipline: replacement holds both
    // m_PresentationLock and the context lock, while every reader/writer
    // holds at least one of them.
    bool m_BlackFrameInsertionCacheValid;
    UINT m_BlackFrameInsertionCacheWidth;
    UINT m_BlackFrameInsertionCacheHeight;
    uint32_t m_BlackFrameInsertionRestoreRejects;
    bool m_BlackFrameInsertionWaitableSwapchain = false;
    bool m_VrrBorderlessFlipModel;
    bool m_VrrSwapChainAllowsTearing;
    bool m_VrrSuspended;
    VrrFallbackReason m_VrrFallbackReason;
    bool m_VrrFramePrepared;
    bool m_VrrContextLocked;
    Microsoft::WRL::ComPtr<ID3D11Fence> m_VrrPresentReadyFence;
    UINT64 m_VrrPresentReadyFenceValue;
    HANDLE m_VrrPresentReadyFenceEvent;
    bool m_VrrPresentReadyAvailable;

    std::array<Microsoft::WRL::ComPtr<ID3D11PixelShader>, PixelShaders::_COUNT> m_VideoPixelShaders;
    Microsoft::WRL::ComPtr<ID3D11Buffer> m_VideoVertexBuffer;
    Microsoft::WRL::ComPtr<ID3D11Buffer> m_BlackFrameInsertionFullscreenVertexBuffer;
    Microsoft::WRL::ComPtr<ID3D11Buffer> m_BlackFrameInsertionBrightConstants;
    Microsoft::WRL::ComPtr<ID3D11Buffer> m_BlackFrameInsertionRecoveryConstants;
    Microsoft::WRL::ComPtr<ID3D11PixelShader> m_BlackFrameInsertionDimPixelShader;

    // Only valid if !m_BindDecoderOutputTextures
    Microsoft::WRL::ComPtr<ID3D11Texture2D> m_VideoTexture;

    // Only index 0 is valid if !m_BindDecoderOutputTextures
    std::vector<std::array<Microsoft::WRL::ComPtr<ID3D11ShaderResourceView>, 2>> m_VideoTextureResourceViews;

    SDL_SpinLock m_OverlayLock;
    std::array<Microsoft::WRL::ComPtr<ID3D11Buffer>, Overlay::OverlayMax> m_OverlayVertexBuffers;
    std::array<Microsoft::WRL::ComPtr<ID3D11Texture2D>, Overlay::OverlayMax> m_OverlayTextures;
    std::array<Microsoft::WRL::ComPtr<ID3D11ShaderResourceView>, Overlay::OverlayMax> m_OverlayTextureResourceViews;
    Microsoft::WRL::ComPtr<ID3D11PixelShader> m_OverlayPixelShader;
    Microsoft::WRL::ComPtr<ID3D11PixelShader> m_BfiOverlayPixelShader;

    AVBufferRef* m_HwDeviceContext;
};
