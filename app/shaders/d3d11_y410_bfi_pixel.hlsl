#include "d3d11_yuv444_bfi_pixel_start.hlsli"

min16float3 swizzle(min16float3 input)
{
    return input.grb;
}

#include "d3d11_yuv444_bfi_pixel_end.hlsli"
