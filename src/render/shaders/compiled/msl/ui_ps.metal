#pragma clang diagnostic ignored "-Wmissing-prototypes"

#include <metal_stdlib>
#include <simd/simd.h>

using namespace metal;

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
    float4 _entryPointOutput [[color(0)]];
};

struct vcs_main_in
{
    float4 i_col [[user(locn0)]];
    float2 i_uv [[user(locn1)]];
};

static inline __attribute__((always_inline))
float4 _main(thread const VSOut& i, texture2d<float> uiTexture, sampler uiSampler)
{
    return i.col * uiTexture.sample(uiSampler, i.uv);
}

fragment vcs_main_out vcs_main(vcs_main_in in [[stage_in]], texture2d<float> uiTexture [[texture(0)]], sampler uiSampler [[sampler(0)]], float4 gl_FragCoord [[position]])
{
    vcs_main_out out = {};
    VSOut i;
    i.pos = gl_FragCoord;
    i.col = in.i_col;
    i.uv = in.i_uv;
    VSOut param = i;
    out._entryPointOutput = _main(param, uiTexture, uiSampler);
    return out;
}

