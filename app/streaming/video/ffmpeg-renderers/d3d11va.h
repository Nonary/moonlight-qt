#pragma once

#include "renderer.h"
#include "pacer/vrrpresenter.h"

#include <d3d11_4.h>
#include <dxgi1_6.h>

extern "C" {
#include <libavutil/hwcontext_d3d11va.h>
}

#include <wrl/client.h>
#include <wrl/wrappers/corewrappers.h>

class D3D11VARenderer : public IFFmpegRenderer
{
public:
    D3D11VARenderer(int decoderSelectionPass);
    virtual ~D3D11VARenderer() override;
    virtual bool initialize(PDECODER_PARAMETERS params) override;
    virtual bool prepareDecoderContext(AVCodecContext* context, AVDictionary**) override;
    virtual bool prepareDecoderContextInGetFormat(AVCodecContext* context, AVPixelFormat pixelFormat) override;
    virtual void renderFrame(AVFrame* frame) override;
    virtual void notifyOverlayUpdated(Overlay::OverlayType) override;
    virtual bool notifyWindowChanged(PWINDOW_STATE_CHANGE_INFO stateInfo) override;
    virtual int getRendererAttributes() override;
    virtual PresentationMode getPresentationMode() override;
    virtual const char* getPresentationModeFallbackReason() override;
    virtual uint64_t popPresentAlignmentWaitUs() override;
    virtual void setPresentTargetUs(uint64_t targetUs, bool catchUp, uint64_t alignBudgetUs, bool vsyncLatch, bool nearBuffered) override;
    virtual uint64_t getLastPresentUs() override;
    virtual uint32_t popMidScanTearCount() override;
    virtual bool isVrrRasterLockUncertain() override;
    virtual int getDecoderCapabilities() override;
    virtual InitFailureReason getInitFailureReason() override;
    virtual void waitToRender() override;

    enum PixelShaders {
        GENERIC_YUV_420,
        GENERIC_AYUV,
        GENERIC_Y410,
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
    bool createVideoTextureSRV(ID3D11Resource* texture,
                               D3D11_SRV_DIMENSION viewDimension,
                               UINT arraySlice,
                               DXGI_FORMAT srvFormat,
                               UINT planeSlice,
                               Microsoft::WRL::ComPtr<ID3D11ShaderResourceView>& srv);
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
    bool queryTearingSupport(HRESULT* result = nullptr);
    void resolvePresentationMode(SDL_Window* window, DXGI_SWAP_CHAIN_DESC1* swapChainDesc);
    void logPresentationMode(SDL_Window* window, const DXGI_SWAP_CHAIN_DESC1* swapChainDesc, int outputIndex, const char* fallbackReason);
    void refreshOutput();
    bool signalAndUnlockForPresent();

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
    int m_AdapterIndex;
    Microsoft::WRL::ComPtr<ID3D11Device5> m_RenderDevice, m_DecodeDevice;
    Microsoft::WRL::ComPtr<ID3D11DeviceContext4> m_RenderDeviceContext, m_DecodeDeviceContext;
    Microsoft::WRL::ComPtr<ID3D11Texture2D> m_RenderSharedTextureArray;
    Microsoft::WRL::ComPtr<IDXGISwapChain4> m_SwapChain;
    Microsoft::WRL::ComPtr<IDXGIOutput> m_Output;

    // Executes the cadence pacer's per-present intent (target hold, blank
    // alignment, latch bookkeeping) and owns the display scanline source.
    // Renderer-agnostic; this class only supplies GPU fencing, the Present
    // call, and display attach notifications.
    VrrPresenter m_VrrPresenter;

    Microsoft::WRL::ComPtr<ID3D11Fence> m_PresentReadyFence;
    uint64_t m_PresentReadyFenceValue;
    HANDLE m_PresentReadyFenceEvent;
    bool m_PresentReadyFenceFailed;
    HANDLE m_FrameLatencyWaitableObject;
    Microsoft::WRL::ComPtr<ID3D11RenderTargetView> m_RenderTargetView;
    Microsoft::WRL::ComPtr<ID3D11BlendState> m_VideoBlendState;
    Microsoft::WRL::ComPtr<ID3D11BlendState> m_OverlayBlendState;

    SupportedFenceType m_FenceType;
    Microsoft::WRL::ComPtr<ID3D11Fence> m_DecodeD2RFence, m_RenderD2RFence;
    UINT64 m_D2RFenceValue;
    Microsoft::WRL::ComPtr<ID3D11Fence> m_DecodeR2DFence, m_RenderR2DFence;
    UINT64 m_R2DFenceValue;
    SDL_mutex* m_ContextLock;
    bool m_BindDecoderOutputTextures;

    DECODER_PARAMETERS m_DecoderParams;
    DXGI_FORMAT m_TextureFormat;
    int m_DisplayWidth;
    int m_DisplayHeight;
    AVColorTransferCharacteristic m_LastColorTrc;

    bool m_AllowTearing;
    PresentationMode m_PresentationMode;
    const char* m_PresentationModeFallbackReason;
    bool m_TearingSupport;
    int m_OutputIndex;

    std::array<Microsoft::WRL::ComPtr<ID3D11PixelShader>, PixelShaders::_COUNT> m_VideoPixelShaders;
    Microsoft::WRL::ComPtr<ID3D11Buffer> m_VideoVertexBuffer;

    // Only valid if !m_BindDecoderOutputTextures
    Microsoft::WRL::ComPtr<ID3D11Texture2D> m_VideoTexture;

    // Only index 0 is valid if !m_BindDecoderOutputTextures
    std::vector<std::array<Microsoft::WRL::ComPtr<ID3D11ShaderResourceView>, 2>> m_VideoTextureResourceViews;

    SDL_SpinLock m_OverlayLock;
    std::array<Microsoft::WRL::ComPtr<ID3D11Buffer>, Overlay::OverlayMax> m_OverlayVertexBuffers;
    std::array<Microsoft::WRL::ComPtr<ID3D11Texture2D>, Overlay::OverlayMax> m_OverlayTextures;
    std::array<Microsoft::WRL::ComPtr<ID3D11ShaderResourceView>, Overlay::OverlayMax> m_OverlayTextureResourceViews;
    Microsoft::WRL::ComPtr<ID3D11PixelShader> m_OverlayPixelShader;

    AVBufferRef* m_HwDeviceContext;
};

