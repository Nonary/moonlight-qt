// Convert SDR display-referred RGB to HDR10 PQ for black frame insertion.
// Drive SDR reference white at 600 nits to preserve roughly 300-nit average
// luminance across the renderer's 50% black/video duty cycle.
cbuffer BFI_CONST_BUF : register(b1)
{
    float bfiReferenceWhiteNits;
    float3 bfiPadding;
};

min16float3 sdrToBfiPq(min16float3 sdr)
{
    float3 sdrFloat = saturate(float3(sdr));
    float3 linearRgb = lerp(sdrFloat / 12.92,
                            pow((sdrFloat + 0.055) / 1.055, 2.4),
                            step(0.04045, sdrFloat));

    const float m1 = 2610.0 / 16384.0;
    const float m2 = 2523.0 / 32.0;
    const float c1 = 3424.0 / 4096.0;
    const float c2 = 2413.0 / 128.0;
    const float c3 = 2392.0 / 128.0;
    float3 scaled = pow(saturate(
        linearRgb * (bfiReferenceWhiteNits / 10000.0)), m1);
    return min16float3(pow((c1 + c2 * scaled) / (1.0 + c3 * scaled), m2));
}
