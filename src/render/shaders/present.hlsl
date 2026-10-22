// The game's picture letterboxed into the target: one quad from the vertex id (triangle strip of 4).
cbuffer Params : register(b0)
{
    float4 rect;  // NDC: left, top, right, bottom
    float2 uvMax; // the used part of the texture
    float2 pad;
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
    o.uv = c * uvMax;
    return o;
}

Texture2D picture : register(t0);
SamplerState pictureSampler : register(s0);

float4 ps_main(VSOut i) : SV_Target
{
    return picture.Sample(pictureSampler, i.uv);
}
