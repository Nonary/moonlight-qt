#pragma once

// Keep the reported parameters and the native call together. This boundary is
// independent of the Windows SDK so a fake swapchain can verify the arguments.
struct DxgiPresentParameters
{
    unsigned int syncInterval;
    unsigned int flags;

    static constexpr DxgiPresentParameters adaptive(bool latched,
                                                    unsigned int tearingFlag,
                                                    bool allowTearing = true)
    {
        return latched ? DxgiPresentParameters{1, 0} :
                         DxgiPresentParameters{0, allowTearing ? tearingFlag : 0};
    }

    template<typename SwapChain>
    auto present(SwapChain& swapChain) const
    {
        return swapChain.Present(syncInterval, flags);
    }
};
