#include "nr_common.hlsli"

Texture2D<float3> Src : register(t0);
RWTexture2D<float3> Dst : register(u0);

float3 Finite(float3 texel)
{
    const bool3 finite = (asuint(texel) & 0x7F800000u) != 0x7F800000u;
    return max(select(finite, texel, select(texel > 0.0, 65504.0, 0.0)), 0.0);
}

[numthreads(8, 8, 1)]
void main(uint3 id : SV_DispatchThreadID)
{
    if (id.x >= g_width || id.y >= g_height)
        return;

    uint source_w, source_h;
    Src.GetDimensions(source_w, source_h);
    const float2 source_size = float2(source_w, source_h);
    const float2 scale = source_size / float2(g_width, g_height);
    const float2 center = (float2(id.xy) + 0.5) * scale;
    const float2 footprint_min = max(center - 0.5 * scale, 0.0);
    const float2 footprint_max = min(center + 0.5 * scale, source_size);
    const int2 first = int2(floor(footprint_min));
    const int2 last = int2(ceil(footprint_max));

    float3 sum = 0.0;
    float weight = 0.0;
    for (int y = first.y; y < last.y; ++y)
    {
        for (int x = first.x; x < last.x; ++x)
        {
            const float2 overlap = min(float2(x + 1, y + 1), footprint_max) - max(float2(x, y), footprint_min);
            const float w = max(overlap.x, 0.0) * max(overlap.y, 0.0);
            sum += Finite(Src.Load(int3(x, y, 0))) * w;
            weight += w;
        }
    }

    Dst[id.xy] = EncodeDisplay(weight > 0.0 ? sum / weight : 0.0, g_white_point);
}
