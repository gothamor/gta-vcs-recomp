#pragma clang diagnostic ignored "-Wmissing-prototypes"
#pragma clang diagnostic ignored "-Wmissing-braces"

#include <metal_stdlib>
#include <simd/simd.h>

using namespace metal;

template<typename T, size_t Num>
struct spvUnsafeArray
{
    T elements[Num ? Num : 1];
    
    thread T& operator [] (size_t pos) thread
    {
        return elements[pos];
    }
    constexpr const thread T& operator [] (size_t pos) const thread
    {
        return elements[pos];
    }
    
    device T& operator [] (size_t pos) device
    {
        return elements[pos];
    }
    constexpr const device T& operator [] (size_t pos) const device
    {
        return elements[pos];
    }
    
    constexpr const constant T& operator [] (size_t pos) const constant
    {
        return elements[pos];
    }
    
    threadgroup T& operator [] (size_t pos) threadgroup
    {
        return elements[pos];
    }
    constexpr const threadgroup T& operator [] (size_t pos) const threadgroup
    {
        return elements[pos];
    }
};

// Implementation of signed integer mod accurate to SPIR-V specification
template<typename Tx, typename Ty>
inline Tx spvSMod(Tx x, Ty y)
{
    Tx remainder = x - y * (x / y);
    return select(Tx(remainder + y), remainder, remainder == 0 || (x >= 0) == (y >= 0));
}

struct VSOut
{
    float4 pos;
    float3 stq;
    float4 col;
    float4 colFlat;
    float fog;
    float depth;
    float2 clip;
};

struct PSOut
{
    float4 color;
    float depth;
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
    float4 _entryPointOutput_color [[color(0)]];
    float gl_FragDepth [[depth(any)]];
};

struct vcs_main_in
{
    float3 i_stq [[user(locn0), center_no_perspective]];
    float4 i_col [[user(locn1), center_no_perspective]];
    float4 i_colFlat [[user(locn2), flat]];
    float i_fog [[user(locn3), center_no_perspective]];
    float i_depth [[user(locn4), center_no_perspective]];
    float gl_ClipDistance_0 [[user(clip0)]];
    float gl_ClipDistance_1 [[user(clip1)]];
};

static inline __attribute__((always_inline))
int wrapc(thread const int& c, thread const int& size, thread const int& mode, thread const int& mn, thread const int& mx)
{
    int r = (c & mn) | mx;
    if (mode == 0)
    {
        if ((size & (size - 1)) == 0)
        {
            r = c & (size - 1);
        }
        else
        {
            r = spvSMod(spvSMod(c, size) + size, size);
        }
    }
    else
    {
        if (mode == 1)
        {
            r = clamp(c, 0, size - 1);
        }
        else
        {
            if (mode == 2)
            {
                r = clamp(c, mn, mx);
            }
        }
    }
    return r;
}

static inline __attribute__((always_inline))
float2 rtScale2(constant Draw& _104)
{
    return float2(_104.targetRtFix.z, _104.rtOrigin.w);
}

static inline __attribute__((always_inline))
float4 fetchTexel(thread int2& t, constant Draw& _104, texture2d<float> tex)
{
    int param = t.x;
    int param_1 = _104.texSizeWrap.x;
    int param_2 = _104.texSizeWrap.z;
    int param_3 = _104.region.x;
    int param_4 = _104.region.y;
    t.x = wrapc(param, param_1, param_2, param_3, param_4);
    int param_5 = t.y;
    int param_6 = _104.texSizeWrap.y;
    int param_7 = _104.texSizeWrap.w;
    int param_8 = _104.region.z;
    int param_9 = _104.region.w;
    t.y = wrapc(param_5, param_6, param_7, param_8, param_9);
    int2 at = t;
    if (_104.atstArefAfailRt.w == 1)
    {
        at = int2(((float2(t) + _104.rtOrigin.xy) + float2(0.5)) * rtScale2(_104));
    }
    return tex.read(uint2(int3(at, 0).xy), int3(at, 0).z) * 255.0;
}

static inline __attribute__((always_inline))
float4 sampleBilinear(thread const float2& uv, constant Draw& _104, texture2d<float> tex)
{
    float2 p = uv - float2(0.5);
    int2 i = int2(floor(p));
    float2 f = p - float2(i);
    int2 param = i;
    float4 _214 = fetchTexel(param, _104, tex);
    float4 c00 = _214;
    int2 param_1 = i + int2(1, 0);
    float4 _220 = fetchTexel(param_1, _104, tex);
    float4 c10 = _220;
    int2 param_2 = i + int2(0, 1);
    float4 _226 = fetchTexel(param_2, _104, tex);
    float4 c01 = _226;
    int2 param_3 = i + int2(1);
    float4 _232 = fetchTexel(param_3, _104, tex);
    float4 c11 = _232;
    return mix(mix(c00, c10, float4(f.x)), mix(c01, c11, float4(f.x)), float4(f.y));
}

static inline __attribute__((always_inline))
float4 sampleTex(thread const float2& uv, constant Draw& _104, texture2d<float> tex)
{
    float2 param = uv;
    return sampleBilinear(param, _104, tex);
}

static inline __attribute__((always_inline))
PSOut _main(thread const VSOut& i, thread bool& g_gatherMismatch, constant Draw& _104, texture2d<float> tex, thread float2& g_pixel)
{
    g_pixel = i.pos.xy;
    PSOut o;
    o.depth = i.depth;
    float4 vc = floor((((_104.linearIipFgeAte.y == 1) ? i.col : i.colFlat) * 255.0) + float4(0.5));
    float4 c = vc;
    if (_104.tmeTfxTccFst.x == 1)
    {
        float2 uv = (_104.tmeTfxTccFst.w == 1) ? i.stq.xy : ((i.stq.xy / float2(i.stq.z)) * float2(_104.texSizeWrap.xy));
        float2 param = uv;
        float4 t = sampleTex(param, _104, tex);
        int tfx = _104.tmeTfxTccFst.y;
        bool tcc = _104.tmeTfxTccFst.z == 1;
        if (tfx == 0)
        {
            float3 _332 = (t.xyz * vc.xyz) / float3(128.0);
            c.x = _332.x;
            c.y = _332.y;
            c.z = _332.z;
            c.w = tcc ? ((t.w * vc.w) / 128.0) : vc.w;
        }
        else
        {
            if (tfx == 1)
            {
                c.x = t.xyz.x;
                c.y = t.xyz.y;
                c.z = t.xyz.z;
                c.w = tcc ? t.w : vc.w;
            }
            else
            {
                if (tfx == 2)
                {
                    float3 _385 = ((t.xyz * vc.xyz) / float3(128.0)) + float3(vc.w);
                    c.x = _385.x;
                    c.y = _385.y;
                    c.z = _385.z;
                    c.w = tcc ? (t.w + vc.w) : vc.w;
                }
                else
                {
                    float3 _413 = ((t.xyz * vc.xyz) / float3(128.0)) + float3(vc.w);
                    c.x = _413.x;
                    c.y = _413.y;
                    c.z = _413.z;
                    c.w = tcc ? t.w : vc.w;
                }
            }
        }
    }
    c = fast::clamp(floor(c), float4(0.0), float4(255.0));
    if (_104.linearIipFgeAte.z == 1)
    {
        float f = floor((i.fog * 255.0) + 0.5);
        float4 _444 = c;
        float3 _458 = floor(((_444.xyz * f) + (_104.fogColor.xyz * (255.0 - f))) / float3(255.0));
        c.x = _458.x;
        c.y = _458.y;
        c.z = _458.z;
    }
    bool passed = true;
    if (_104.linearIipFgeAte.w == 1)
    {
        float a = c.w;
        float r = float(_104.atstArefAfailRt.y);
        int atst = _104.atstArefAfailRt.x;
        if (atst == 0)
        {
            passed = false;
        }
        else
        {
            if (atst == 2)
            {
                passed = a < r;
            }
            else
            {
                if (atst == 3)
                {
                    passed = a <= r;
                }
                else
                {
                    if (atst == 4)
                    {
                        passed = a == r;
                    }
                    else
                    {
                        if (atst == 5)
                        {
                            passed = a >= r;
                        }
                        else
                        {
                            if (atst == 6)
                            {
                                passed = a > r;
                            }
                            else
                            {
                                if (atst == 7)
                                {
                                    passed = a != r;
                                }
                            }
                        }
                    }
                }
            }
        }
    }
    if ((_104.atstArefAfailRt.z == 0) && (!passed))
    {
        discard_fragment();
    }
    if ((_104.atstArefAfailRt.z == 1) && passed)
    {
        discard_fragment();
    }
    int bc = _104.blendCPremul.x;
    float cf = (bc == 0) ? (c.w / 128.0) : ((bc == 2) ? (_104.targetRtFix.w / 128.0) : 1.0);
    float3 rgb = c.xyz;
    if (_104.blendCPremul.y == 1)
    {
        rgb *= cf;
    }
    else
    {
        if (_104.blendCPremul.y == 2)
        {
            rgb *= (1.0 + cf);
        }
    }
    o.color = float4(rgb / float3(255.0), c.w / 255.0);
    if (g_gatherMismatch)
    {
        o.color = float4(1.0, 0.0, 1.0, 1.0);
    }
    return o;
}

fragment vcs_main_out vcs_main(vcs_main_in in [[stage_in]], constant Draw& _104 [[buffer(0)]], texture2d<float> tex [[texture(0)]], float4 gl_FragCoord [[position]])
{
    vcs_main_out out = {};
    spvUnsafeArray<float, 2> gl_ClipDistance = {};
    gl_ClipDistance[0] = in.gl_ClipDistance_0;
    gl_ClipDistance[1] = in.gl_ClipDistance_1;
    bool g_gatherMismatch = false;
    VSOut i;
    i.pos = gl_FragCoord;
    i.stq = in.i_stq;
    i.col = in.i_col;
    i.colFlat = in.i_colFlat;
    i.fog = in.i_fog;
    i.depth = in.i_depth;
    i.clip.x = gl_ClipDistance[0];
    i.clip.y = gl_ClipDistance[1];
    VSOut param = i;
    float2 g_pixel;
    PSOut _641 = _main(param, g_gatherMismatch, _104, tex, g_pixel);
    PSOut flattenTemp = _641;
    out._entryPointOutput_color = flattenTemp.color;
    out.gl_FragDepth = flattenTemp.depth;
    return out;
}

