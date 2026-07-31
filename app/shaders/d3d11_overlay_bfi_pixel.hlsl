Texture2D<min16float4> theTexture : register(t0);
SamplerState theSampler : register(s0);

struct ShaderInput
{
    float4 pos : SV_POSITION;
    float2 tex : TEXCOORD0;
};

#include "d3d11_bfi.hlsli"

min16float4 main(ShaderInput input) : SV_TARGET
{
    min16float4 color = theTexture.Sample(theSampler, input.tex);
    return min16float4(sdrToBfiPq(color.rgb), color.a);
}
