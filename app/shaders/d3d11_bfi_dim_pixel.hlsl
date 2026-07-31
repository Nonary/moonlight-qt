Texture2D<min16float4> theTexture : register(t0);
SamplerState theSampler : register(s0);

struct ShaderInput
{
    float4 pos : SV_POSITION;
    float2 tex : TEXCOORD0;
};

min16float4 main(ShaderInput input) : SV_TARGET
{
    min16float4 color = theTexture.Sample(theSampler, input.tex);
    float3 pq = saturate(float3(color.rgb));

    const float m1 = 2610.0 / 16384.0;
    const float m2 = 2523.0 / 32.0;
    const float c1 = 3424.0 / 4096.0;
    const float c2 = 2413.0 / 128.0;
    const float c3 = 2392.0 / 128.0;

    // Decode the cached 600-nit HDR10 image to linear light, halve its
    // luminance, then encode it back to PQ. Applying 0.5 directly to the PQ
    // signal would be far darker than the 300-nit recovery frame.
    float3 encodedPower = pow(pq, 1.0 / m2);
    float3 linearRgb = pow(
        max(encodedPower - c1, 0.0) /
            (c2 - c3 * encodedPower),
        1.0 / m1);
    linearRgb *= 0.5;

    float3 linearPower = pow(max(linearRgb, 0.0), m1);
    float3 dimmedPq = pow(
        (c1 + c2 * linearPower) /
            (1.0 + c3 * linearPower),
        m2);
    return min16float4(dimmedPq, color.a);
}
