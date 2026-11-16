// GX vertex shader: a pass-through. Transform, lighting and texgen run on the
// CPU (gx_xform.cpp); this only hands the results to the rasterizer.
//
// Input is struct GXVtxOut (gx_internal.h). SDL3 GPU wants vertex inputs as
// TEXCOORD{location}: 0 pos, 1 clr0, 2 clr1, 3..10 tex0..tex7.
// Compiled by tools/build_shaders.bat.

#include "gx_io.hlsli"

struct GXVertex {
    float4 pos : TEXCOORD0;  // clip space, SDL GPU convention (NDC +Y up, Z 0..1)
    float4 clr0 : TEXCOORD1; // 0..1, lit and clamped
    float4 clr1 : TEXCOORD2;
    float3 tex0 : TEXCOORD3; // (s, t, q)
    float3 tex1 : TEXCOORD4;
    float3 tex2 : TEXCOORD5;
    float3 tex3 : TEXCOORD6;
    float3 tex4 : TEXCOORD7;
    float3 tex5 : TEXCOORD8;
    float3 tex6 : TEXCOORD9;
    float3 tex7 : TEXCOORD10;
};

GXVaryings main(GXVertex v) {
    GXVaryings o;
    o.pos = v.pos;
    o.clr0 = v.clr0;
    o.clr1 = v.clr1;
    o.tex0 = v.tex0;
    o.tex1 = v.tex1;
    o.tex2 = v.tex2;
    o.tex3 = v.tex3;
    o.tex4 = v.tex4;
    o.tex5 = v.tex5;
    o.tex6 = v.tex6;
    o.tex7 = v.tex7;
    return o;
}
