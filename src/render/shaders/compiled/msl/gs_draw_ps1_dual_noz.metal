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
    float4 blend;
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
    float4 _entryPointOutput_blend [[color(0), index(1)]];
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
PSOut _main(thread const VSOut& i, thread bool& g_gatherMismatch, thread float2& g_pixel, constant Draw& _33)
{
    g_pixel = i.pos.xy;
    float4 vc = floor((((_33.linearIipFgeAte.y == 1) ? i.col : i.colFlat) * 255.0) + float4(0.5));
    float4 c = vc;
    c = fast::clamp(floor(c), float4(0.0), float4(255.0));
    if (_33.linearIipFgeAte.z == 1)
    {
        float f = floor((i.fog * 255.0) + 0.5);
        float4 _76 = c;
        float3 _91 = floor(((_76.xyz * f) + (_33.fogColor.xyz * (255.0 - f))) / float3(255.0));
        c.x = _91.x;
        c.y = _91.y;
        c.z = _91.z;
    }
    bool passed = true;
    if (_33.linearIipFgeAte.w == 1)
    {
        float a = c.w;
        float r = float(_33.atstArefAfailRt.y);
        int atst = _33.atstArefAfailRt.x;
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
    if ((_33.atstArefAfailRt.z == 0) && (!passed))
    {
        discard_fragment();
    }
    if ((_33.atstArefAfailRt.z == 1) && passed)
    {
        discard_fragment();
    }
    int bc = _33.blendCPremul.x;
    float cf = (bc == 0) ? (c.w / 128.0) : ((bc == 2) ? (_33.targetRtFix.w / 128.0) : 1.0);
    float3 rgb = c.xyz;
    if (_33.blendCPremul.y == 1)
    {
        rgb *= cf;
    }
    else
    {
        if (_33.blendCPremul.y == 2)
        {
            rgb *= (1.0 + cf);
        }
    }
    PSOut o;
    o.color = float4(rgb / float3(255.0), c.w / 255.0);
    if (g_gatherMismatch)
    {
        o.color = float4(1.0, 0.0, 1.0, 1.0);
    }
    o.blend = float4(fast::min(cf, 1.0));
    return o;
}

fragment vcs_main_out vcs_main(vcs_main_in in [[stage_in]], constant Draw& _33 [[buffer(0)]], float4 gl_FragCoord [[position]])
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
    PSOut _289 = _main(param, g_gatherMismatch, g_pixel, _33);
    PSOut flattenTemp = _289;
    out._entryPointOutput_color = flattenTemp.color;
    out._entryPointOutput_blend = flattenTemp.blend;
    return out;
}

