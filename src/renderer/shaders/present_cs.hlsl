// Copies the game's frontbuffer into the presenter's guest output
// (R10G10B10A2_UNORM, written as UAV) through the display gamma ramp the game
// set (gamma_ramp.h). Compiled at build time with dxc.
Texture2D<float4> g_Source : register(t0);
RWTexture2D<float4> g_Dest : register(u0);
cbuffer Gamma : register(b0)
{
    uint g_Mode;  // 0: 256-entry table, 1: piecewise linear
    uint3 g_Pad;
    uint4 g_Ramp[256];
};

float3 ApplyGamma(float3 color)
{
    color = saturate(color);
    float3 result;
    if (g_Mode == 0)
    {
        uint3 i = uint3(color * 255.0 + 0.5);
        result = float3((g_Ramp[i.r].x >> 20) & 0x3FF, (g_Ramp[i.g].x >> 10) & 0x3FF, g_Ramp[i.b].x & 0x3FF) / 1023.0;
    }
    else
    {
        // 128 segments of 8: base + delta * (the low 3 bits) / 8, 16-bit.
        uint3 i = uint3(color * 1023.0 + 0.5);
        [unroll] for (uint c = 0; c < 3; c++)
        {
            uint entry = g_Ramp[i[c] >> 3][c];
            result[c] = min((entry & 0xFFFF) + (((entry >> 16) * (i[c] & 7)) >> 3), 0xFFFF) / 65535.0;
        }
    }
    return result;
}

[numthreads(8, 8, 1)]
void main(uint3 id : SV_DispatchThreadID)
{
    uint width, height;
    g_Dest.GetDimensions(width, height);
    if (id.x < width && id.y < height)
        g_Dest[id.xy] = float4(ApplyGamma(g_Source[id.xy].rgb), 1.0);
}
