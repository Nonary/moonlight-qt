#pragma once

// Keep the reported parameters and the native call together. This boundary is
// independent of the Windows SDK so a fake swapchain can verify the arguments.
struct DxgiPresentParameters
{
    unsigned int syncInterval;
    unsigned int flags;

    static constexpr DxgiPresentParameters adaptive(bool latched,
                                                    unsigned int tearingFlag,
                                                    bool vrr12Protection = false)
    {
        return latched ? DxgiPresentParameters{vrr12Protection ? 0u : 1u, 0} :
                         DxgiPresentParameters{0, tearingFlag};
    }

    constexpr bool protectedPresentation(unsigned int tearingFlag) const
    {
        return syncInterval != 0 || (flags & tearingFlag) == 0;
    }

    template<typename SwapChain>
    auto present(SwapChain& swapChain) const
    {
        return swapChain.Present(syncInterval, flags);
    }
};
