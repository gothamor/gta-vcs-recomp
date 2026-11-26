#pragma clang diagnostic ignored "-Wmissing-prototypes"

#include <metal_stdlib>
#include <simd/simd.h>

using namespace metal;

struct VSOut
{
    float4 pos;
    float2 uv;
};

struct Quad
{
    float4 rect;
    float4 uvRect;
    float4 color;
    float4 params;
};

struct vcs_main_out
{
    float2 _entryPointOutput_uv [[user(locn0)]];
    float4 gl_Position [[position]];
};

static inline __attribute__((always_inline))
VSOut _main(thread const uint& id, constant Quad& _33)
{
    float2 c = float2(float(id & 1u), float(id >> uint(1)));
    VSOut o;
    o.pos = float4(mix(_33.rect.x, _33.rect.z, c.x), mix(_33.rect.y, _33.rect.w, c.y), 0.0, 1.0);
    o.uv = mix(_33.uvRect.xy, _33.uvRect.zw, c);
    return o;
}

vertex vcs_main_out vcs_main(constant Quad& _33 [[buffer(0)]], uint gl_VertexIndex [[vertex_id]])
{
    vcs_main_out out = {};
    uint id = gl_VertexIndex;
    uint param = id;
    VSOut flattenTemp = _main(param, _33);
    out.gl_Position = flattenTemp.pos;
    out._entryPointOutput_uv = flattenTemp.uv;
    return out;
}

