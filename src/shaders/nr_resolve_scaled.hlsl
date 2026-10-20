#include "nr_common.hlsli"

Texture2D<float3> Orig : register(t0);
Texture2D<float3> Model : register(t1);
Texture2D<float3> Proxy : register(t2);
RWTexture2D<float3> Out : register(u0);

static const float kRatioFloor = 1.0 / 65536.0;
static const float kGuideFloor = 1e-8;
static const float kSpatialSigma = 0.5;
static const float kRangeSigma = 0.5;
static const int kTile = 20;

groupshared float4 tile_edit[kTile * kTile];

uint TileIndex(int2 local)
{
    const int2 at = clamp(local, 0, kTile - 1);
    return uint(at.y * kTile + at.x);
}

[numthreads(16, 16, 1)]
void main(uint3 id : SV_DispatchThreadID, uint3 group_id : SV_GroupID, uint group_index : SV_GroupIndex)
{
    uint nr_w, nr_h;
    Model.GetDimensions(nr_w, nr_h);
    const int2 last = int2(nr_w, nr_h) - 1;
    const float2 to_nr = float2(nr_w, nr_h) / float2(g_width, g_height);
    const int2 origin = int2(floor((float2(group_id.xy * 16u) + 0.5) * to_nr - 0.5)) - 1;

    for (uint i = group_index; i < uint(kTile * kTile); i += 256u)
    {
        const int2 tap = clamp(origin + int2(i % uint(kTile), i / uint(kTile)), 0, last);
        const float3 proxy = DecodeDisplay(Proxy.Load(int3(tap, 0)), 1.0);
        const float3 neural = DecodeDisplay(Model.Load(int3(tap, 0)), 1.0);
        tile_edit[i] = float4(log2(max(neural, kRatioFloor)) - log2(max(proxy, kRatioFloor)),
                              log2(max(Luma(proxy), kGuideFloor)));
    }
    GroupMemoryBarrierWithGroupSync();

    if (id.x >= g_width || id.y >= g_height)
        return;

    const float white_point = max(g_white_point, 1e-4);
    const float3 o = max(Orig[id.xy], 0.0);
    const float3 o_normalised = o / white_point;
    const float2 position = (float2(id.xy) + 0.5) * to_nr - 0.5;
    const int2 base = int2(floor(position));
    const float guide_full = log2(max(Luma(o_normalised), kGuideFloor));

    float3 joint = 0.0;
    float joint_weight = 0.0;
    float3 spatial = 0.0;
    float spatial_weight = 0.0;
    for (int dy = -1; dy <= 2; ++dy)
    {
        for (int dx = -1; dx <= 2; ++dx)
        {
            const int2 tap = base + int2(dx, dy);
            const float2 distance = float2(tap) - position;
            const float ws = exp(-dot(distance, distance) / (2.0 * kSpatialSigma * kSpatialSigma));
            const float4 edit = tile_edit[TileIndex(tap - origin)];
            const float difference = guide_full - edit.a;
            const float wr = exp(-(difference * difference) / (2.0 * kRangeSigma * kRangeSigma));
            joint += ws * wr * edit.rgb;
            joint_weight += ws * wr;
            spatial += ws * edit.rgb;
            spatial_weight += ws;
        }
    }
    const float3 edit_up = joint_weight > 1e-6 ? joint / joint_weight : spatial / spatial_weight;
    const float3 gain = exp2(edit_up);
    const float3 m_normalised = select(o_normalised >= kRatioFloor, o_normalised * gain,
                                       o_normalised + kRatioFloor * (gain - 1.0));
    const float3 m = max(m_normalised, 0.0) * white_point;

    const int2 nearest = clamp(int2((float2(id.xy) + 0.5) * to_nr), 0, last);
    if (g_debug == 1)
    {
        Out[id.xy] = Proxy.Load(int3(nearest, 0));
        return;
    }
    if (g_debug == 2)
    {
        Out[id.xy] = Model.Load(int3(nearest, 0));
        return;
    }
    if (g_debug == 3)
    {
        Out[id.xy] = abs(m - o) * 20.0;
        return;
    }

    Out[id.xy] = Anchor(o, m);
}
