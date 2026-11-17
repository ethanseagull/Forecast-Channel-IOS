// Presents the EFB on the window with the screen fill (port/gx/gx_sdl.cpp,
// compose()): the EFB's colour times the vertex colour, with the vertex
// alpha for blending (the 3D margins fade in over the edge extension). Runs
// after gx_vs.hlsl, so it reads the same varyings: tex0.xy the EFB texture
// coordinates, tex1.xy the range u is clamped to (the frame's own columns, so
// its edge never filters in a margin's texel), clr0.
//
// Edge extension quads (tex2.x = 1; gx_sdl.cpp edge_params) continue each
// row's colour trend outward: the average colour at the frame's edge and
// ~17 game pixels further in give a slope, applied over the distance from
// the frame (tex2.y, game pixels) with an ease-off so it levels out. Flat
// bands (button bars) stay flat; gradients (the forecast panel) keep going.
// tex0.xy is the frame's edge column, tex2.z the inward u step per game
// pixel, tex3.x the v step per game pixel (rows are averaged over 3).
// Compiled by tools/build_shaders.bat (DXIL) and tools/build_shaders_msl.sh.

#include "gx_io.hlsli"

Texture2D<float4> efbTex : register(t0, space2);
SamplerState efbSmp : register(s0, space2);

#define EDGE_INNER 17.0 // game pixels between the two column groups
#define EDGE_EASE 55.0  // game pixels: the trend levels out over about this
#define DIVIDER_W 5.6   // game pixels: the button divider at the frame's edge

float3 Tap(float2 uv) {
    return efbTex.Sample(efbSmp, uv).rgb;
}

float4 main(GXVaryings i) : SV_Target0 {
    if (i.tex2.x > 0.5) {
        float du = i.tex2.z, dv = i.tex3.x;
        float3 edge = float3(0.0, 0.0, 0.0), inner = float3(0.0, 0.0, 0.0);
        [unroll] for (int r = -1; r <= 1; r++) {
            float v = i.tex0.y + (float)r * dv;
            edge += Tap(float2(i.tex0.x + 0.5 * du, v)) + Tap(float2(i.tex0.x + 2.5 * du, v));
            inner += Tap(float2(i.tex0.x + (EDGE_INNER + 0.5) * du, v)) +
                     Tap(float2(i.tex0.x + (EDGE_INNER + 2.5) * du, v));
        }
        edge *= 1.0 / 6.0;
        inner *= 1.0 / 6.0;
        float t = EDGE_EASE * (1.0 - exp(-i.tex2.y / EDGE_EASE));
        float3 c = saturate(edge + (edge - inner) * (t / EDGE_INNER));

        // The button bars (the channel's own limits: y < 63 and y > 393 of
        // 456) are grey. Stretching their outer buttons would leave those
        // buttons' labels off centre, so the bar ends in a divider at the
        // frame's edge and the strip beyond reads as a blank button.
        float gameY = i.tex0.y / dv;
        float lo = min(edge.r, min(edge.g, edge.b)), hi = max(edge.r, max(edge.g, edge.b));
        if ((gameY < 63.0 || gameY > 393.0) && hi - lo < 0.08 && hi > 0.45) {
            // The divider in screen order (left to right), game pixels:
            // highlight, dark core, mid grey, as the channel draws it.
            float s = du > 0.0 ? DIVIDER_W - i.tex2.y : i.tex2.y;
            float k = s < 0.0 ? 1.0 : s < 1.0 ? 1.08 : s < 1.9 ? 1.16 : s < 3.4 ? 0.5 : s < 5.2 ? 0.83 : s < DIVIDER_W ? 0.92 : 1.0;
            c = saturate(edge * k);
        }
        return float4(c * i.clr0.rgb, i.clr0.a);
    }
    float2 uv = float2(clamp(i.tex0.x, i.tex1.x, i.tex1.y), i.tex0.y);
    float3 c = efbTex.Sample(efbSmp, uv).rgb;
    return float4(c * i.clr0.rgb, i.clr0.a);
}
