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
float2 rtScale2(constant Draw& _116)
{
    return float2(_116.targetRtFix.z, _116.rtOrigin.w);
}

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
float4 fetchRtFull(thread const int2& c, constant Draw& _116, texture2d<float> tex)
{
    float2 s = rtScale2(_116);
    int2 t = int2(floor(((float2(c) + float2(0.5)) / s) - _116.rtOrigin.xy));
    int param = t.x;
    int param_1 = _116.texSizeWrap.x;
    int param_2 = _116.texSizeWrap.z;
    int param_3 = _116.region.x;
    int param_4 = _116.region.y;
    int2 w;
    w.x = wrapc(param, param_1, param_2, param_3, param_4);
    int param_5 = t.y;
    int param_6 = _116.texSizeWrap.y;
    int param_7 = _116.texSizeWrap.w;
    int param_8 = _116.region.z;
    int param_9 = _116.region.w;
    w.y = wrapc(param_5, param_6, param_7, param_8, param_9);
    int2 lo = int2(ceil(((float2(w) + _116.rtOrigin.xy) * s) - float2(0.5)));
    int2 hi = int2(ceil((((float2(w) + _116.rtOrigin.xy) + float2(1.0)) * s) - float2(0.5))) - int2(1);
    int2 at = clamp(c, lo, max(lo, hi));
    return tex.read(uint2(int3(at, 0).xy), int3(at, 0).z) * 255.0;
}

static inline __attribute__((always_inline))
float4 sampleRtFull(thread const float2& uv, constant Draw& _116, texture2d<float> tex)
{
    float2 h = (uv + _116.rtOrigin.xy) * rtScale2(_116);
    if (_116.linearIipFgeAte.x == 0)
    {
        int2 param = int2(floor(h));
        return fetchRtFull(param, _116, tex);
    }
    float2 p = h - float2(0.5);
    int2 i = int2(floor(p));
    float2 f = p - float2(i);
    int2 param_1 = i;
    float4 c00 = fetchRtFull(param_1, _116, tex);
    int2 param_2 = i + int2(1, 0);
    float4 c10 = fetchRtFull(param_2, _116, tex);
    int2 param_3 = i + int2(0, 1);
    float4 c01 = fetchRtFull(param_3, _116, tex);
    int2 param_4 = i + int2(1);
    float4 c11 = fetchRtFull(param_4, _116, tex);
    return mix(mix(c00, c10, float4(f.x)), mix(c01, c11, float4(f.x)), float4(f.y));
}

static inline __attribute__((always_inline))
float4 fetchTexel(thread int2& t, constant Draw& _116, texture2d<float> tex)
{
    int param = t.x;
    int param_1 = _116.texSizeWrap.x;
    int param_2 = _116.texSizeWrap.z;
    int param_3 = _116.region.x;
    int param_4 = _116.region.y;
    t.x = wrapc(param, param_1, param_2, param_3, param_4);
    int param_5 = t.y;
    int param_6 = _116.texSizeWrap.y;
    int param_7 = _116.texSizeWrap.w;
    int param_8 = _116.region.z;
    int param_9 = _116.region.w;
    t.y = wrapc(param_5, param_6, param_7, param_8, param_9);
    int2 at = t;
    if (_116.atstArefAfailRt.w == 1)
    {
        at = int2(((float2(t) + _116.rtOrigin.xy) + float2(0.5)) * rtScale2(_116));
    }
    return tex.read(uint2(int3(at, 0).xy), int3(at, 0).z) * 255.0;
}

static inline __attribute__((always_inline))
float4 sampleBilinear(thread const float2& uv, constant Draw& _116, texture2d<float> tex)
{
    float2 p = uv - float2(0.5);
    int2 i = int2(floor(p));
    float2 f = p - float2(i);
    int2 param = i;
    float4 _590 = fetchTexel(param, _116, tex);
    float4 c00 = _590;
    int2 param_1 = i + int2(1, 0);
    float4 _595 = fetchTexel(param_1, _116, tex);
    float4 c10 = _595;
    int2 param_2 = i + int2(0, 1);
    float4 _600 = fetchTexel(param_2, _116, tex);
    float4 c01 = _600;
    int2 param_3 = i + int2(1);
    float4 _605 = fetchTexel(param_3, _116, tex);
    float4 c11 = _605;
    return mix(mix(c00, c10, float4(f.x)), mix(c01, c11, float4(f.x)), float4(f.y));
}

static inline __attribute__((always_inline))
float4 sampleRtFiltered(thread const float2& uv, constant Draw& _116, texture2d<float> tex, thread float2& g_pixel)
{
    float2 ds = fast::max(_116.filterDest.xy, float2(1.0));
    float2 P = floor(g_pixel);
    float2 t = floor((P + float2(0.5)) / ds);
    if (any(floor((t + float2(0.5)) * ds) != P))
    {
        float2 param = uv;
        return sampleBilinear(param, _116, tex);
    }
    float2 s = rtScale2(_116);
    float2 halfSize = fast::max(_116.filterSize.xy, float2(0.5));
    uint2 sizeQueryTemp = uint2(tex.get_width(), tex.get_height());
    uint tw = sizeQueryTemp.x;
    uint th = sizeQueryTemp.y;
    int2 maxPx = min(int2(int(tw), int(th)), int2(ceil(_116.filterSize.zw * s))) - int2(1);
    int2 p0 = clamp(int2(ceil((((uv - halfSize) + _116.rtOrigin.xy) * s) - float2(0.5))), int2(0), maxPx);
    int2 p1 = clamp(int2(floor((((uv + halfSize) + _116.rtOrigin.xy) * s) - float2(0.5))), p0, maxPx);
    int2 _step = max(int2(1), (((p1 - p0) + int2(16)) / int2(16)));
    float4 sum = float4(0.0);
    float weights = 0.0;
    for (int y = p0.y; y <= p1.y; y += _step.y)
    {
        float wy = fast::max(0.0, 1.0 - (abs((((float(y) + 0.5) / s.y) - _116.rtOrigin.y) - uv.y) / halfSize.y));
        for (int x = p0.x; x <= p1.x; x += _step.x)
        {
            float w = wy * fast::max(0.0, 1.0 - (abs((((float(x) + 0.5) / s.x) - _116.rtOrigin.x) - uv.x) / halfSize.x));
            sum += (tex.read(uint2(int3(x, y, 0).xy), int3(x, y, 0).z) * w);
            weights += w;
        }
    }
    int2 param_1 = int2(floor(uv));
    float4 _843 = fetchTexel(param_1, _116, tex);
    return (weights > 0.0) ? (sum * (255.0 / weights)) : _843;
}

static inline __attribute__((always_inline))
float4 sampleGather(thread const float2& uv, thread bool& g_gatherMismatch, constant Draw& _116, texture2d<float> tex, sampler hwSampler)
{
    float2 p = uv - float2(0.5);
    int2 i = int2(floor(p));
    float2 f = p - floor(p);
    int2 size = _116.texSizeWrap.xy;
    int2 w;
    w.x = (_116.texSizeWrap.z == 0) ? (i.x & (size.x - 1)) : clamp(i.x, -1, size.x - 1);
    w.y = (_116.texSizeWrap.w == 0) ? (i.y & (size.y - 1)) : clamp(i.y, -1, size.y - 1);
    float2 at = (float2(w) + float2(1.0)) / float2(size);
    float4 gr = tex.gather(hwSampler, at, int2(0), component::x);
    float4 gg = tex.gather(hwSampler, at, int2(0), component::y);
    float4 gb = tex.gather(hwSampler, at, int2(0), component::z);
    float4 ga = tex.gather(hwSampler, at, int2(0), component::w);
    float4 c00 = float4(gr.w, gg.w, gb.w, ga.w) * 255.0;
    float4 c10 = float4(gr.z, gg.z, gb.z, ga.z) * 255.0;
    float4 c01 = float4(gr.x, gg.x, gb.x, ga.x) * 255.0;
    float4 c11 = float4(gr.y, gg.y, gb.y, ga.y) * 255.0;
    if (_116.linearIipFgeAte.x == 4)
    {
        int2 param = i;
        float4 _528 = fetchTexel(param, _116, tex);
        int2 param_1 = i + int2(1, 0);
        float4 _536 = fetchTexel(param_1, _116, tex);
        int2 param_2 = i + int2(0, 1);
        float4 _544 = fetchTexel(param_2, _116, tex);
        int2 param_3 = i + int2(1);
        float4 _552 = fetchTexel(param_3, _116, tex);
        g_gatherMismatch = ((any(c00 != _528) || any(c10 != _536)) || any(c01 != _544)) || any(c11 != _552);
    }
    return mix(mix(c00, c10, float4(f.x)), mix(c01, c11, float4(f.x)), float4(f.y));
}

static inline __attribute__((always_inline))
float4 sampleTex(thread const float2& uv, thread bool& g_gatherMismatch, constant Draw& _116, texture2d<float> tex, sampler hwSampler, thread float2& g_pixel)
{
    float4 r;
    if (_116.atstArefAfailRt.w == 2)
    {
        float2 param = uv;
        r = sampleRtFull(param, _116, tex);
    }
    else
    {
        if (_116.atstArefAfailRt.w == 3)
        {
            float2 param_1 = uv;
            r = sampleRtFiltered(param_1, _116, tex, g_pixel);
        }
        else
        {
            if (_116.linearIipFgeAte.x == 2)
            {
                r = tex.sample(hwSampler, (uv / float2(_116.texSizeWrap.xy))) * 255.0;
            }
            else
            {
                if (_116.linearIipFgeAte.x >= 3)
                {
                    float2 param_2 = uv;
                    float4 _890 = sampleGather(param_2, g_gatherMismatch, _116, tex, hwSampler);
                    r = _890;
                }
                else
                {
                    if (_116.linearIipFgeAte.x == 0)
                    {
                        int2 param_3 = int2(floor(uv));
                        float4 _901 = fetchTexel(param_3, _116, tex);
                        r = _901;
                    }
                    else
                    {
                        float2 param_4 = uv;
                        r = sampleBilinear(param_4, _116, tex);
                    }
                }
            }
        }
    }
    return r;
}

static inline __attribute__((always_inline))
PSOut _main(thread const VSOut& i, thread bool& g_gatherMismatch, constant Draw& _116, texture2d<float> tex, sampler hwSampler, thread float2& g_pixel)
{
    g_pixel = i.pos.xy;
    PSOut o;
    o.depth = i.depth;
    float4 vc = floor((((_116.linearIipFgeAte.y == 1) ? i.col : i.colFlat) * 255.0) + float4(0.5));
    float4 c = vc;
    if (_116.tmeTfxTccFst.x == 1)
    {
        float2 uv = (_116.tmeTfxTccFst.w == 1) ? i.stq.xy : ((i.stq.xy / float2(i.stq.z)) * float2(_116.texSizeWrap.xy));
        float2 param = uv;
        float4 _961 = sampleTex(param, g_gatherMismatch, _116, tex, hwSampler, g_pixel);
        float4 t = _961;
        int tfx = _116.tmeTfxTccFst.y;
        bool tcc = _116.tmeTfxTccFst.z == 1;
        if (tfx == 0)
        {
            float3 _981 = (t.xyz * vc.xyz) / float3(128.0);
            c.x = _981.x;
            c.y = _981.y;
            c.z = _981.z;
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
                    float3 _1034 = ((t.xyz * vc.xyz) / float3(128.0)) + float3(vc.w);
                    c.x = _1034.x;
                    c.y = _1034.y;
                    c.z = _1034.z;
                    c.w = tcc ? (t.w + vc.w) : vc.w;
                }
                else
                {
                    float3 _1062 = ((t.xyz * vc.xyz) / float3(128.0)) + float3(vc.w);
                    c.x = _1062.x;
                    c.y = _1062.y;
                    c.z = _1062.z;
                    c.w = tcc ? t.w : vc.w;
                }
            }
        }
    }
    c = fast::clamp(floor(c), float4(0.0), float4(255.0));
    if (_116.linearIipFgeAte.z == 1)
    {
        float f = floor((i.fog * 255.0) + 0.5);
        float4 _1091 = c;
        float3 _1105 = floor(((_1091.xyz * f) + (_116.fogColor.xyz * (255.0 - f))) / float3(255.0));
        c.x = _1105.x;
        c.y = _1105.y;
        c.z = _1105.z;
    }
    bool passed = true;
    if (_116.linearIipFgeAte.w == 1)
    {
        float a = c.w;
        float r = float(_116.atstArefAfailRt.y);
        int atst = _116.atstArefAfailRt.x;
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
    if ((_116.atstArefAfailRt.z == 0) && (!passed))
    {
        discard_fragment();
    }
    if ((_116.atstArefAfailRt.z == 1) && passed)
    {
        discard_fragment();
    }
    int bc = _116.blendCPremul.x;
    float cf = (bc == 0) ? (c.w / 128.0) : ((bc == 2) ? (_116.targetRtFix.w / 128.0) : 1.0);
    float3 rgb = c.xyz;
    if (_116.blendCPremul.y == 1)
    {
        rgb *= cf;
    }
    else
    {
        if (_116.blendCPremul.y == 2)
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

fragment vcs_main_out vcs_main(vcs_main_in in [[stage_in]], constant Draw& _116 [[buffer(0)]], texture2d<float> tex [[texture(0)]], sampler hwSampler [[sampler(0)]], float4 gl_FragCoord [[position]])
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
    PSOut _1287 = _main(param, g_gatherMismatch, _116, tex, hwSampler, g_pixel);
    PSOut flattenTemp = _1287;
    out._entryPointOutput_color = flattenTemp.color;
    out.gl_FragDepth = flattenTemp.depth;
    return out;
}

