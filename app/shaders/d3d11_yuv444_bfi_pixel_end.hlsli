min16float4 main(ShaderInput input) : SV_TARGET
{
    min16float3 yuv = swizzle(videoTex.Sample(theSampler, input.tex));
    yuv -= offsets;
    return min16float4(sdrToBfiPq(mul(yuv, cscMatrix)), 1.0);
}
