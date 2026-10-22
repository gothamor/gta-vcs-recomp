// Helper passes for the HW GS renderer (gs_hw.cpp): textured blits between targets and solid fills (partial clears).
// Both draw one quad from the vertex id (triangle strip of 4) inside `rect` (NDC: left, top, right, bottom).

cbuffer Quad : register(b0)
{
    float4 rect;
    float4 uvRect;   // blit: source texture coordinates (0..1) at the left/top and right/bottom edges
    float4 color;    // fill: colour 0..1
    float4 params;   // x: fill depth 0..1, y: blit writes alpha 1 (the displayed picture)
};

struct VSOut
{
    float4 pos : SV_Position;
    float2 uv : TEXCOORD0;
};

VSOut vs_main(uint id : SV_VertexID)
{
    const float2 c = float2(id & 1, id >> 1);
    VSOut o;
    o.pos = float4(lerp(rect.x, rect.z, c.x), lerp(rect.y, rect.w, c.y), 0.0, 1.0);
    o.uv = lerp(uvRect.xy, uvRect.zw, c);
    return o;
}

Texture2D<float4> source : register(t0);
SamplerState sourceSampler : register(s0);

float4 ps_blit(VSOut i) : SV_Target0
{
    const float4 t = source.Sample(sourceSampler, i.uv);
    return float4(t.rgb, params.y > 0.5 ? 1.0 : t.a);
}

float4 ps_fill_color(VSOut i) : SV_Target0
{
    return color;
}

float ps_fill_depth(VSOut i) : SV_Depth
{
    return params.x;
}
