// Dear ImGui's draw lists through the RHI (render/ui_renderer.cpp): the backends without an ImGui renderer of their own.
cbuffer Ui : register(b0)
{
    float4 scaleOffset; // ImGui display coordinates -> NDC: xy scale, zw offset
};

struct VSIn
{
    float2 pos : POSITION;
    float2 uv : TEXCOORD0;
    float4 col : COLOR0;
};

struct VSOut
{
    float4 pos : SV_Position;
    float4 col : COLOR0;
    float2 uv : TEXCOORD0;
};

VSOut vs_main(VSIn i)
{
    VSOut o;
    o.pos = float4(i.pos * scaleOffset.xy + scaleOffset.zw, 0.0, 1.0);
    o.col = i.col;
    o.uv = i.uv;
    return o;
}

Texture2D uiTexture : register(t0);
SamplerState uiSampler : register(s0);

float4 ps_main(VSOut i) : SV_Target
{
    return i.col * uiTexture.Sample(uiSampler, i.uv);
}
