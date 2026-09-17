// R = coverage; G = device depth, or nearest-depth encoding when flag bit 1 is set.

Texture2D<float2> AllyMask : register(t0);
Texture2D<float> SceneDepth : register(t1);
Texture2D<float2> LocalMask : register(t2);

cbuffer OutlineStyle : register(b0)
{
    uint2 Dimensions;
    float Thickness;
    float DepthBias;
    float3 AshWhite;
    float Opacity;
    uint ReversedDepth;
    uint VisibleOutline;
    uint ShowMask;
    uint FillSilhouette;
    float FillOpacity;
    float3 Padding;
};

float4 FullscreenVS(uint vertex : SV_VertexID) : SV_Position
{
    float2 p = float2((vertex << 1) & 2, vertex & 2);
    return float4(p * float2(2, -2) + float2(-1, 1), 0, 1);
}

bool OnScreen(int2 p)
{
    return all(p >= 0) && all(p < int2(Dimensions));
}

float Coverage(int2 p)
{
    if (!OnScreen(p)) return 1;
    return saturate(AllyMask.Load(int3(p, 0)).x);
}

float4 OutlinePS(float4 position : SV_Position) : SV_Target
{
    int2 p = int2(position.xy);
    float2 ally = AllyMask.Load(int3(p, 0));
    float scene = SceneDepth.Load(int3(p, 0));
    if (!all(isfinite(ally)) || !isfinite(scene) || ally.x <= 0 ||
        ally.y < 0 || ally.y > 1 || scene < 0 || scene > 1) discard;

    if ((ReversedDepth & 2) != 0 && (ReversedDepth & 1) == 0) ally.y = 1 - ally.y;
    // Only the visible local player's surface blocks the ally overlay. A wall
    // in front of both actors must still reveal the ally through that wall.
    if ((ReversedDepth & 4) != 0)
    {
        float2 local = LocalMask.Load(int3(p, 0));
        if (local.x > 0 && all(isfinite(local)) && local.y >= 0 && local.y <= 1)
        {
            if ((ReversedDepth & 2) != 0 && (ReversedDepth & 1) == 0) local.y = 1 - local.y;
            float localBehindScene = (ReversedDepth & 1) != 0 ? scene - local.y : local.y - scene;
            float allyBehindLocal = (ReversedDepth & 1) != 0 ? local.y - ally.y : ally.y - local.y;
            if (localBehindScene <= DepthBias && allyBehindLocal > DepthBias) discard;
        }
    }
    float behind = (ReversedDepth & 1) != 0 ? scene - ally.y : ally.y - scene;
    if (ShowMask != 0) return float4(AshWhite * Opacity, Opacity);
    if (VisibleOutline == 0 && behind <= DepthBias) discard;

    const int2 directions[8] = {
        int2(1, 0), int2(-1, 0), int2(0, 1), int2(0, -1),
        int2(1, 1), int2(-1, 1), int2(1, -1), int2(-1, -1)
    };
    float edge = 0;
    [loop] for (int radius = 1; radius <= 6; ++radius)
    {
        [unroll] for (int i = 0; i < 8; ++i)
        {
            float distance = radius * (i < 4 ? 1.0 : 1.41421356);
            float falloff = 1 - smoothstep(Thickness, Thickness + 1.25, distance);
            if (falloff > 0)
                edge = max(edge, (1 - Coverage(p + radius * directions[i])) * falloff);
        }
    }
    float alpha = saturate(ally.x) * edge * Opacity;
    if (FillSilhouette != 0 && behind > DepthBias)
    {
        alpha = max(alpha, saturate(ally.x) * FillOpacity);
    }
    if (alpha <= 0) discard;
    return float4(AshWhite * alpha, alpha);
}
