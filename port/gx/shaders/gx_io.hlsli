// The interface between gx_vs.hlsl and gx_ps.hlsl: what the vertex shader
// passes on and the pixel shader reads. Both must agree on it exactly (D3D12
// matches the two signatures by semantic and order).
#ifndef GX_IO_HLSLI
#define GX_IO_HLSLI

struct GXVaryings {
    float4 pos : SV_Position;
    // The two rasterized colour channels, 0..1.
    float4 clr0 : TEXCOORD0;
    float4 clr1 : TEXCOORD1;
    // Texture coordinates (s, t, q) 0-7; the pixel shader divides by q.
    float3 tex0 : TEXCOORD2;
    float3 tex1 : TEXCOORD3;
    float3 tex2 : TEXCOORD4;
    float3 tex3 : TEXCOORD5;
    float3 tex4 : TEXCOORD6;
    float3 tex5 : TEXCOORD7;
    float3 tex6 : TEXCOORD8;
    float3 tex7 : TEXCOORD9;
};

#endif
