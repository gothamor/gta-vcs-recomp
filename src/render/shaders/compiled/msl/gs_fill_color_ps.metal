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
    float4 _entryPointOutput [[color(0)]];
};

struct vcs_main_in
{
    float2 i_uv [[user(locn0)]];
};

static inline __attribute__((always_inline))
float4 _main(thread const VSOut& i, constant Quad& _17)
{
    return _17.color;
}

fragment vcs_main_out vcs_main(vcs_main_in in [[stage_in]], constant Quad& _17 [[buffer(0)]], float4 gl_FragCoord [[position]])
{
    vcs_main_out out = {};
    VSOut i;
    i.pos = gl_FragCoord;
    i.uv = in.i_uv;
    VSOut param = i;
    out._entryPointOutput = _main(param, _17);
    return out;
}

