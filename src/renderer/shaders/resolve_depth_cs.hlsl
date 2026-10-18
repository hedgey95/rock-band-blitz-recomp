// Depth resolve: copies a rectangle of the depth plane of a depth target into
// an R32_FLOAT texture (the game's k_24_8_FLOAT destination), which D3D12 copies
// cannot do for part of a depth-stencil resource. Compiled at build time with dxc.
#ifdef MULTISAMPLED
Texture2DMS<float> g_Source : register(t0);
#else
Texture2D<float> g_Source : register(t0);
#endif
RWTexture2D<float> g_Dest : register(u0);
cbuffer Rect : register(b0)
{
    int2 g_SourceOrigin;
    int2 g_DestOrigin;
    uint2 g_Size;
};

[numthreads(8, 8, 1)]
void main(uint3 id : SV_DispatchThreadID)
{
    if (id.x < g_Size.x && id.y < g_Size.y)
#ifdef MULTISAMPLED
        g_Dest[g_DestOrigin + int2(id.xy)] = g_Source.Load(g_SourceOrigin + int2(id.xy), 0);
#else
        g_Dest[g_DestOrigin + int2(id.xy)] = g_Source[g_SourceOrigin + int2(id.xy)];
#endif
}
