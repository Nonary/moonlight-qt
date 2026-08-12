// Convert SDR display-referred RGB to HDR10 PQ for black frame insertion.
// Drive SDR reference white at 600 nits to preserve roughly 300-nit average
// luminance across the renderer's 50% black/video duty cycle.
cbuffer BFI_CONST_BUF : register(b1)
{
    float bfiReferenceWhiteNits;
    float3 bfiPadding;
    float4 bfiSourceToBt2020R;
    float4 bfiSourceToBt2020G;
    float4 bfiSourceToBt2020B;
};

min16float3 sdrToBfiPq(min16float3 sdr)
{
    float3 sdrFloat = saturate(float3(sdr));
    // BFI receives the same SDR RGB signal that the old path converted with
    // the sRGB EOTF. Keep that transfer fixed for game captures: honoring a
    // BT.709 metadata tag here lifts midtones and produces a washed-out HDR
    // result even though the capture values themselves are unchanged.
    float3 sourceLinearRgb = lerp(sdrFloat / 12.92,
                                  pow((sdrFloat + 0.055) / 1.055, 2.4),
                                  step(0.04045, sdrFloat));

    // The swapchain is tagged as BT.2020 PQ. Convert gamut in linear light
    // before applying the common luminance scale and PQ encoding; scaling the
    // nonlinear channels or skipping this matrix changes hue and saturation.
    float3 linearRgb = float3(
        dot(sourceLinearRgb, bfiSourceToBt2020R.xyz),
        dot(sourceLinearRgb, bfiSourceToBt2020G.xyz),
        dot(sourceLinearRgb, bfiSourceToBt2020B.xyz));
    linearRgb = max(linearRgb, 0.0);

    const float m1 = 2610.0 / 16384.0;
    const float m2 = 2523.0 / 32.0;
    const float c1 = 3424.0 / 4096.0;
    const float c2 = 2413.0 / 128.0;
    const float c3 = 2392.0 / 128.0;
    float3 scaled = pow(saturate(
        linearRgb * (bfiReferenceWhiteNits / 10000.0)), m1);
    return min16float3(pow((c1 + c2 * scaled) / (1.0 + c3 * scaled), m2));
}
