#pragma once

#include "presentationclock.h"
#include "compositionpresentpolicy.h"

#include <d3d11_4.h>
#include <dcomp.h>
#include <presentation.h>
#include <wrl/client.h>
#include <array>
#include <deque>

// A presentation manager owns directly displayable render targets. The caller
// uses its existing shaders and GPU-ready fence, then enqueues its intended
// display deadline. This class never waits for a refresh or a statistics event.
// All methods are serialized by the renderer's context lock.
class D3D11CompositionPresenter {
public:
    struct DisplayedFrame {
        uint64_t id = 0;
        PresentationClockSample clock;
        uint64_t presentDuration100ns = 0;
    };
    struct PresentStatusFrame {
        uint64_t id = 0;
        PresentStatus status = PresentStatus_Queued;
    };
    struct PresentResult {
        uint64_t requestedTargetUs = 0;
        uint64_t targetTime100ns = 0;
        uint64_t clockUncertaintyUs = 0;
        uint64_t referenceTime100ns = 0;
        uint64_t clockBeforeUs = 0;
        uint64_t clockAfterUs = 0;
        uint64_t submissionTimeUs = 0;
        uint64_t submissionEndUs = 0;
        unsigned outstandingPresents = 0;
    };

    ~D3D11CompositionPresenter();
    static bool runtimeSupported();
    static bool deviceSupported(ID3D11Device* device);
    HRESULT initialize(ID3D11Device* device, HWND window, UINT width, UINT height,
                       DXGI_FORMAT format, LUID adapter, UINT sourceId);
    void reset();
    bool active() const { return m_Manager != nullptr; }
    HRESULT resize(UINT width, UINT height, DXGI_FORMAT format);
    HRESULT acquire(ID3D11RenderTargetView** view);
    void cancel() { m_Acquired = m_Buffers.size(); }
    HRESULT clearQueuedPresents();
    HRESULT setColorSpace(DXGI_COLOR_SPACE_TYPE colorSpace);
    HRESULT present(uint64_t& id);
    HRESULT present(uint64_t targetUs, uint64_t (*clockUs)(), uint64_t& id,
                    PresentResult* result = nullptr);
    bool pollDisplayedFrame(uint64_t (*clockUs)(), DisplayedFrame& frame);
    bool pollPresentStatus(PresentStatusFrame& frame);
    uint64_t composedFrames() const { return m_ComposedFrames; }
    uint64_t independentFrames() const { return m_IndependentFrames; }
    uint64_t rejectedDisplayFrames() const { return m_RejectedDisplayFrames; }
    uint64_t queuedFrames() const { return m_QueuedFrames; }
    uint64_t skippedFrames() const { return m_SkippedFrames; }
    uint64_t canceledFrames() const { return m_CanceledFrames; }
    uint64_t droppedStatusFrames() const { return m_DroppedStatusFrames; }
    uint64_t retiringPresentId() const { return m_RetiringPresentId; }
    unsigned outstandingPresents() const { return m_Ownership.outstanding(); }

private:
    struct Buffer {
        Microsoft::WRL::ComPtr<ID3D11Texture2D> texture;
        Microsoft::WRL::ComPtr<ID3D11RenderTargetView> view;
        Microsoft::WRL::ComPtr<IPresentationBuffer> presentation;
    };
    struct NativeDisplayedFrame {
        uint64_t id = 0;
        uint64_t time100ns = 0;
        uint64_t duration100ns = 0;
    };
    void drainStatistics();
    HRESULT refreshOwnership();
    // Buffer storage is not a queue target. Only available buffers are used;
    // accepted presents retain their native display/retirement lifecycle.
    std::array<Buffer, 5> m_Buffers;
    CompositionPresentOwnership<5> m_Ownership;
    std::deque<NativeDisplayedFrame> m_DisplayedFrames;
    std::deque<PresentStatusFrame> m_StatusFrames;
    size_t m_Acquired = m_Buffers.size();
    size_t m_NextBuffer = 0;
    HMODULE m_Module = nullptr;
    HANDLE m_SurfaceHandle = nullptr;
    HANDLE m_LostEvent = nullptr;
    Microsoft::WRL::ComPtr<ID3D11Device> m_Device;
    Microsoft::WRL::ComPtr<IDCompositionDevice> m_Composition;
    Microsoft::WRL::ComPtr<IDCompositionTarget> m_Target;
    Microsoft::WRL::ComPtr<IDCompositionVisual> m_Visual;
    Microsoft::WRL::ComPtr<IPresentationManager> m_Manager;
    Microsoft::WRL::ComPtr<ID3D11Fence> m_RetiringFence;
    Microsoft::WRL::ComPtr<IPresentationSurface> m_Surface;
    LUID m_Adapter = {};
    UINT m_SourceId = 0;
    uint64_t m_LastDisplayedId = 0;
    uint64_t m_LastObservedDisplayId = 0;
    uint64_t m_RetiringPresentId = 0;
    uint64_t m_ComposedFrames = 0;
    uint64_t m_IndependentFrames = 0;
    uint64_t m_RejectedDisplayFrames = 0;
    uint64_t m_QueuedFrames = 0;
    uint64_t m_SkippedFrames = 0;
    uint64_t m_CanceledFrames = 0;
    uint64_t m_DroppedStatusFrames = 0;
};
