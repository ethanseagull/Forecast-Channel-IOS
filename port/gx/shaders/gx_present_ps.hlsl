// Presents the EFB on the window with the screen fill (port/gx/gx_sdl.cpp,
// compose()): the EFB's colour times the vertex colour, with the vertex
// alpha for blending (the 3D margins fade in over the edge extension). Runs
// after gx_vs.hlsl, so it reads the same varyings: tex0.xy the EFB texture
// coordinates, tex1.xy the range u is clamped to (the frame's own columns, so
// its edge never filters in a margin's texel), clr0.
// Compiled by tools/build_shaders.bat (DXIL) and tools/build_shaders_msl.sh.

#include "gx_io.hlsli"

Texture2D<float4> efbTex : register(t0, space2);
SamplerState efbSmp : register(s0, space2);

float4 main(GXVaryings i) : SV_Target0 {
    float2 uv = float2(clamp(i.tex0.x, i.tex1.x, i.tex1.y), i.tex0.y);
    float3 c = efbTex.Sample(efbSmp, uv).rgb;
    return float4(c * i.clr0.rgb, i.clr0.a);
}
