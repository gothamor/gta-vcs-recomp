#pragma clang diagnostic ignored "-Wmissing-prototypes"

#include <metal_stdlib>
#include <simd/simd.h>

using namespace metal;

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

struct WorldIn
{
    float3 pos;
    float3 uv;
    uint col;
};

struct Native
{
    float4 nat[32];
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
    float3 i_uv [[attribute(1)]];
    uint i_col [[attribute(2)]];
};

static inline __attribute__((always_inline))
void nativePosition(thread const float4& S, thread const float4& C, thread const float3& st, thread VSOut& o, constant Native& _62, constant Draw& _91)
{
    float q = 1.0 / S.w;
    float3 s = S.xyz * q;
    float2 xy = (trunc(s.xy * 16.0) / float2(16.0)) - _62.nat[31].xy;
    float z = floor(trunc(s.z * 16.0) / 16.0);
    float depth = fast::min(z * _62.nat[31].z, 1.0);
    float2 p = (xy + float2(_91.rtOrigin.z)) / _91.targetRtFix.xy;
    o.pos = float4(((p.x * 2.0) - 1.0) * S.w, (1.0 - (p.y * 2.0)) * S.w, depth * S.w, S.w);
    o.depth = depth;
    o.clip = float2(C.z + C.w, C.w - C.z);
    o.stq = st * q;
    o.psize = 1.0;
}

static inline __attribute__((always_inline))
float nativeFog(thread const float4& fog, thread const float& w)
{
    float f = 0.0;
    if (fog.x > 0.5)
    {
        f = fast::clamp((fog.z * w) + fog.y, 0.0, 255.0);
    }
    return floor(trunc(f * 16.0) / 16.0) / 255.0;
}

static inline __attribute__((always_inline))
VSOut _main(thread const WorldIn& i, constant Native& _62, constant Draw& _91)
{
    VSOut o = VSOut{ float4(0.0), float3(0.0), float4(0.0), float4(0.0), 0.0, 0.0, float2(0.0), 0.0 };
    float4 S = (((_62.nat[0] * i.pos.x) + (_62.nat[1] * i.pos.y)) + (_62.nat[2] * i.pos.z)) + _62.nat[3];
    float4 C = (((_62.nat[11] * i.pos.x) + (_62.nat[12] * i.pos.y)) + (_62.nat[13] * i.pos.z)) + _62.nat[14];
    float4 param = S;
    float4 param_1 = C;
    float3 param_2 = ((_62.nat[4].xyz * i.uv.x) + (_62.nat[5].xyz * i.uv.y)) + (_62.nat[6].xyz * i.uv.z);
    VSOut param_3 = o;
    nativePosition(param, param_1, param_2, param_3, _62, _91);
    o = param_3;
    float4 c = float4(float(i.col & 255u), float((i.col >> uint(8)) & 255u), float((i.col >> uint(16)) & 255u), float(i.col >> uint(24)));
    o.col = trunc(fast::min(_62.nat[8] + (_62.nat[7] * c), _62.nat[9])) / float4(255.0);
    o.colFlat = o.col;
    float4 param_4 = _62.nat[10];
    float param_5 = S.w;
    o.fog = nativeFog(param_4, param_5);
    return o;
}

vertex vcs_main_out vcs_main(vcs_main_in in [[stage_in]], constant Draw& _91 [[buffer(0)]], constant Native& _62 [[buffer(1)]])
{
    vcs_main_out out = {};
    bool g_gatherMismatch = false;
    WorldIn i;
    i.pos = in.i_pos;
    i.uv = in.i_uv;
    i.col = in.i_col;
    WorldIn param = i;
    VSOut flattenTemp = _main(param, _62, _91);
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

