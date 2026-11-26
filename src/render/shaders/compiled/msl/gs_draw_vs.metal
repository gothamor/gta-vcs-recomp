#pragma clang diagnostic ignored "-Wmissing-prototypes"

#include <metal_stdlib>
#include <simd/simd.h>

using namespace metal;

struct VSIn
{
    float3 pos;
    float3 stq;
    float4 col;
    float fog;
};

struct VSOut
{
    float4 pos;
    float3 stq;
    float4 col;
    float4 colFlat;
    float fog;
    float depth;
    float2 clip;
    float psize;
};

struct Draw
{
    float4 targetRtFix;
    int4 tmeTfxTccFst;
    int4 linearIipFgeAte;
    int4 atstArefAfailRt;
    int4 texSizeWrap;
    int4 region;
    float4 rtOrigin;
    int4 blendCPremul;
    float4 fogColor;
    float4 filterSize;
    float4 filterDest;
};

struct Native
{
    float4 nat[32];
};

struct vcs_main_out
{
    float3 _entryPointOutput_stq [[user(locn0)]];
    float4 _entryPointOutput_col [[user(locn1)]];
    float4 _entryPointOutput_colFlat [[user(locn2)]];
    float _entryPointOutput_fog [[user(locn3)]];
    float _entryPointOutput_depth [[user(locn4)]];
    float4 gl_Position [[position]];
    float gl_PointSize [[point_size]];
    float gl_ClipDistance [[clip_distance]] [2];
    float gl_ClipDistance_0 [[user(clip0)]];
    float gl_ClipDistance_1 [[user(clip1)]];
};

struct vcs_main_in
{
    float3 i_pos [[attribute(0)]];
    float3 i_stq [[attribute(1)]];
    float4 i_col [[attribute(2)]];
    float i_fog [[attribute(3)]];
};

static inline __attribute__((always_inline))
VSOut _main(thread const VSIn& i, constant Draw& _32)
{
    float2 p = (i.pos.xy + float2(_32.rtOrigin.z)) / _32.targetRtFix.xy;
    VSOut o;
    o.pos = float4((p.x * 2.0) - 1.0, 1.0 - (p.y * 2.0), i.pos.z, 1.0);
    o.depth = i.pos.z;
    o.stq = i.stq;
    o.col = i.col;
    o.colFlat = i.col;
    o.fog = i.fog;
    o.clip = float2(1.0);
    o.psize = 1.0;
    return o;
}

vertex vcs_main_out vcs_main(vcs_main_in in [[stage_in]], constant Draw& _32 [[buffer(0)]])
{
    vcs_main_out out = {};
    bool g_gatherMismatch = false;
    VSIn i;
    i.pos = in.i_pos;
    i.stq = in.i_stq;
    i.col = in.i_col;
    i.fog = in.i_fog;
    VSIn param = i;
    VSOut flattenTemp = _main(param, _32);
    out.gl_Position = flattenTemp.pos;
    out._entryPointOutput_stq = flattenTemp.stq;
    out._entryPointOutput_col = flattenTemp.col;
    out._entryPointOutput_colFlat = flattenTemp.colFlat;
    out._entryPointOutput_fog = flattenTemp.fog;
    out._entryPointOutput_depth = flattenTemp.depth;
    out.gl_ClipDistance[0] = flattenTemp.clip.x;
    out.gl_ClipDistance[1] = flattenTemp.clip.y;
    out.gl_PointSize = flattenTemp.psize;
    float2 g_pixel;
    out.gl_ClipDistance_0 = out.gl_ClipDistance[0];
    out.gl_ClipDistance_1 = out.gl_ClipDistance[1];
    return out;
}

