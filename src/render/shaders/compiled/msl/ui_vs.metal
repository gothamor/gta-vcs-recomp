#pragma clang diagnostic ignored "-Wmissing-prototypes"

#include <metal_stdlib>
#include <simd/simd.h>

using namespace metal;

struct VSIn
{
    float2 pos;
    float2 uv;
    float4 col;
};

struct VSOut
{
    float4 pos;
    float4 col;
    float2 uv;
};

struct Ui
{
    float4 scaleOffset;
};

struct vcs_main_out
{
    float4 _entryPointOutput_col [[user(locn0)]];
    float2 _entryPointOutput_uv [[user(locn1)]];
    float4 gl_Position [[position]];
};

struct vcs_main_in
{
    float2 i_pos [[attribute(0)]];
    float2 i_uv [[attribute(1)]];
    float4 i_col [[attribute(2)]];
};

static inline __attribute__((always_inline))
VSOut _main(thread const VSIn& i, constant Ui& _25)
{
    VSOut o;
    o.pos = float4((i.pos * _25.scaleOffset.xy) + _25.scaleOffset.zw, 0.0, 1.0);
    o.col = i.col;
    o.uv = i.uv;
    return o;
}

vertex vcs_main_out vcs_main(vcs_main_in in [[stage_in]], constant Ui& _25 [[buffer(0)]])
{
    vcs_main_out out = {};
    VSIn i;
    i.pos = in.i_pos;
    i.uv = in.i_uv;
    i.col = in.i_col;
    VSIn param = i;
    VSOut flattenTemp = _main(param, _25);
    out.gl_Position = flattenTemp.pos;
    out._entryPointOutput_col = flattenTemp.col;
    out._entryPointOutput_uv = flattenTemp.uv;
    return out;
}

