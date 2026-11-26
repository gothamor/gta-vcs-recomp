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
    float4 position;
    float2 pixel;
    float2 uv;
    float4 rect;
    float4 radii;
    float4 color0;
    float4 color1;
    float4 border_color;
    uint kind;
    float4 params;
    float4 data;
    float4 clip;
};

struct Frame
{
    float2 viewport_size;
    float2 atlas_inv_size;
};

struct vcs_main_out
{
    float4 _entryPointOutput [[color(0)]];
};

struct vcs_main_in
{
    float2 i_pixel [[user(locn0)]];
    float2 i_uv [[user(locn1)]];
    float4 i_rect [[user(locn2), flat]];
    float4 i_radii [[user(locn3), flat]];
    float4 i_color0 [[user(locn4), flat]];
    float4 i_color1 [[user(locn5), flat]];
    float4 i_border_color [[user(locn6), flat]];
    uint i_kind [[user(locn7)]];
    float4 i_params [[user(locn8), flat]];
    float4 i_data [[user(locn9), flat]];
    float4 i_clip [[user(locn10), flat]];
};

static inline __attribute__((always_inline))
float sd_segment(thread const float2& p, thread const float2& a, thread const float2& b)
{
    float2 pa = p - a;
    float2 ba = b - a;
    float h = fast::clamp(dot(pa, ba) / fast::max(dot(ba, ba), 9.9999999747524270787835121154785e-07), 0.0, 1.0);
    return length(pa - (ba * h));
}

static inline __attribute__((always_inline))
float sd_arc(thread const float2& p, thread const float& start, thread const float& sweep, thread const float& r)
{
    if (sweep >= 6.282185077667236328125)
    {
        return abs(length(p) - r);
    }
    float a = precise::atan2(p.y, p.x) - start;
    a -= (6.283185482025146484375 * floor(a / 6.283185482025146484375));
    if (a <= sweep)
    {
        return abs(length(p) - r);
    }
    float2 e0 = float2(cos(start), sin(start)) * r;
    float2 e1 = float2(cos(start + sweep), sin(start + sweep)) * r;
    return fast::min(length(p - e0), length(p - e1));
}

static inline __attribute__((always_inline))
float blurred_glyph(thread const float2& texel, thread const float4& glyph_texels, thread const float& radius, texture2d<float> glyph_atlas, sampler atlas_sampler, constant Frame& _470)
{
    float sigma = radius * 0.5;
    float _step = radius / 3.0;
    float sum = 0.0;
    float weights = 0.0;
    for (int y = -3; y <= 3; y++)
    {
        for (int x = -3; x <= 3; x++)
        {
            float2 o = float2(float(x), float(y)) * _step;
            float w = exp((-dot(o, o)) / ((2.0 * sigma) * sigma));
            float2 t = texel + o;
            float inside = float(all(t >= glyph_texels.xy) && all(t <= glyph_texels.zw));
            sum += ((w * inside) * glyph_atlas.sample(atlas_sampler, (t * _470.atlas_inv_size), level(0.0)).x);
            weights += w;
        }
    }
    return sum / weights;
}

static inline __attribute__((always_inline))
float text_coverage(thread float& a, thread const float3& color)
{
    a = fast::clamp(a, 0.0, 1.0);
    float luma = dot(color, float3(0.2125999927520751953125, 0.715200006961822509765625, 0.072200000286102294921875));
    float light = powr(a, 0.68965518474578857421875);
    float dark = 1.0 - powr(1.0 - a, 0.869565188884735107421875);
    return mix(dark, light, luma);
}

static inline __attribute__((always_inline))
float2 erf2(thread float2& x)
{
    float2 s = sign(x);
    float2 a = abs(x);
    x = float2(1.0) + ((float2(0.2783930003643035888671875) + ((float2(0.23038899898529052734375) + ((a * a) * 0.07810799777507781982421875)) * a)) * a);
    x *= x;
    return s - (s / (x * x));
}

static inline __attribute__((always_inline))
float shadow_x(thread const float& x, thread const float& y, thread const float& sigma, thread const float& corner, thread const float2& half_size)
{
    float delta = fast::min((half_size.y - corner) - abs(y), 0.0);
    float curved = (half_size.x - corner) + sqrt(fast::max(0.0, (corner * corner) - (delta * delta)));
    float2 param = (float2(x) + float2(-curved, curved)) * (0.707106769084930419921875 / sigma);
    float2 _230 = erf2(param);
    float2 integral = float2(0.5) + (_230 * 0.5);
    return integral.y - integral.x;
}

static inline __attribute__((always_inline))
float gaussian(thread const float& x, thread const float& sigma)
{
    return exp((-(x * x)) / ((2.0 * sigma) * sigma)) / (2.5066282749176025390625 * sigma);
}

static inline __attribute__((always_inline))
float rounded_box_shadow(thread const float2& p, thread const float2& half_size, thread const float& corner, thread const float& sigma)
{
    float low = p.y - half_size.y;
    float high = p.y + half_size.y;
    float start = fast::clamp((-3.0) * sigma, low, high);
    float end = fast::clamp(3.0 * sigma, low, high);
    float _step = (end - start) / 4.0;
    float y = start + (_step * 0.5);
    float value = 0.0;
    for (int i = 0; i < 4; i++)
    {
        float param = p.x;
        float param_1 = p.y - y;
        float param_2 = sigma;
        float param_3 = corner;
        float2 param_4 = half_size;
        float param_5 = y;
        float param_6 = sigma;
        value += ((shadow_x(param, param_1, param_2, param_3, param_4) * gaussian(param_5, param_6)) * _step);
        y += _step;
    }
    return value;
}

static inline __attribute__((always_inline))
float dither(thread const float2& pixel)
{
    return (fract(52.98291778564453125 * fract(dot(pixel, float2(0.067110560834407806396484375, 0.005837149918079376220703125)))) - 0.5) / 255.0;
}

static inline __attribute__((always_inline))
float sd_round_box(thread const float2& p, thread const float2& half_size, thread const float4& radii)
{
    float r = (p.x < 0.0) ? ((p.y < 0.0) ? radii.x : radii.w) : ((p.y < 0.0) ? radii.y : radii.z);
    r = fast::min(r, fast::min(half_size.x, half_size.y));
    float2 q = (abs(p) - half_size) + float2(r);
    return (fast::min(fast::max(q.x, q.y), 0.0) + length(fast::max(q, float2(0.0)))) - r;
}

static inline __attribute__((always_inline))
float4 shade(thread const VSOut& i, texture2d<float> glyph_atlas, sampler atlas_sampler, constant Frame& _470)
{
    if (i.kind == 3u)
    {
        float2 param = i.pixel;
        float2 param_1 = i.data.xy;
        float2 param_2 = i.data.zw;
        float d = sd_segment(param, param_1, param_2) - (i.params.z * 0.5);
        float a = fast::clamp(0.5 - d, 0.0, 1.0) * i.color0.w;
        return float4(i.color0.xyz * a, a);
    }
    if (i.kind == 5u)
    {
        spvUnsafeArray<float2, 4> _627 = spvUnsafeArray<float2, 4>({ i.data.xy, i.data.zw, i.radii.xy, i.radii.zw });
        spvUnsafeArray<float2, 4> q = _627;
        float area = 0.0;
        for (int k = 0; k < 4; k++)
        {
            area += ((q[k].x * q[spvSMod(k + 1, 4)].y) - (q[spvSMod(k + 1, 4)].x * q[k].y));
        }
        float side = (area > 0.0) ? 1.0 : (-1.0);
        float d_1 = -1000000000.0;
        for (int k_1 = 0; k_1 < 4; k_1++)
        {
            float2 e = q[spvSMod(k_1 + 1, 4)] - q[k_1];
            float2 outward = (float2(e.y, -e.x) * side) / float2(fast::max(length(e), 9.9999997473787516355514526367188e-06));
            d_1 = fast::max(d_1, dot(i.pixel - q[k_1], outward));
        }
        float a_1 = fast::clamp(0.5 - d_1, 0.0, 1.0) * i.color0.w;
        return float4(i.color0.xyz * a_1, a_1);
    }
    if (i.kind == 4u)
    {
        float2 centre = (i.rect.xy + i.rect.zw) * 0.5;
        float2 param_3 = i.pixel - centre;
        float param_4 = i.data.x;
        float param_5 = i.data.y;
        float param_6 = i.data.z;
        float d_2 = sd_arc(param_3, param_4, param_5, param_6) - (i.data.w * 0.5);
        float a_2 = fast::clamp(0.5 - d_2, 0.0, 1.0) * i.color0.w;
        return float4(i.color0.xyz * a_2, a_2);
    }
    if (i.kind == 1u)
    {
        float a_3;
        if (i.params.y > 0.0)
        {
            float2 param_7 = i.uv / _470.atlas_inv_size;
            float4 param_8 = i.data;
            float param_9 = i.params.y;
            a_3 = blurred_glyph(param_7, param_8, param_9, glyph_atlas, atlas_sampler, _470);
        }
        else
        {
            a_3 = glyph_atlas.sample(atlas_sampler, i.uv).x;
        }
        float param_10 = a_3;
        float3 param_11 = i.color0.xyz;
        float _821 = text_coverage(param_10, param_11);
        a_3 = _821 * i.color0.w;
        return float4(i.color0.xyz * a_3, a_3);
    }
    float2 centre_1 = (i.rect.xy + i.rect.zw) * 0.5;
    float2 half_size = (i.rect.zw - i.rect.xy) * 0.5;
    float2 p = i.pixel - centre_1;
    if (i.kind == 2u)
    {
        float sigma = fast::max(i.params.y, 0.5);
        float2 param_12 = p;
        float2 param_13 = half_size;
        float param_14 = fast::min(i.radii.x, fast::min(half_size.x, half_size.y));
        float param_15 = sigma;
        float a_4 = rounded_box_shadow(param_12, param_13, param_14, param_15) * i.color0.w;
        float2 param_16 = i.pixel;
        a_4 = fast::clamp(a_4 + dither(param_16), 0.0, 1.0);
        return float4(i.color0.xyz * a_4, a_4);
    }
    float2 param_17 = p;
    float2 param_18 = half_size;
    float4 param_19 = i.radii;
    float d_3 = sd_round_box(param_17, param_18, param_19);
    float t = (i.params.w > 0.5) ? fast::clamp((i.pixel.x - i.rect.x) / fast::max(i.rect.z - i.rect.x, 1.0), 0.0, 1.0) : fast::clamp((i.pixel.y - i.rect.y) / fast::max(i.rect.w - i.rect.y, 1.0), 0.0, 1.0);
    float4 fill = mix(i.color0, i.color1, float4(t));
    if (any(i.color0 != i.color1))
    {
        float2 param_20 = i.pixel;
        float4 _968 = fill;
        float3 _971 = _968.xyz + float3(dither(param_20));
        fill.x = _971.x;
        fill.y = _971.y;
        fill.z = _971.z;
    }
    float border = i.params.x;
    float outer = fast::clamp(0.5 - d_3, 0.0, 1.0);
    float inner = fast::clamp(0.5 - (d_3 + border), 0.0, 1.0);
    float4 fill_pm = float4(fill.xyz * fill.w, fill.w);
    float4 border_pm = float4(i.border_color.xyz * i.border_color.w, i.border_color.w);
    return mix(border_pm, fill_pm, float4((border > 0.0) ? inner : 1.0)) * outer;
}

static inline __attribute__((always_inline))
float4 _main(thread const VSOut& i, texture2d<float> glyph_atlas, sampler atlas_sampler, constant Frame& _470)
{
    float2 cover = fast::clamp(fast::min(i.pixel - i.clip.xy, i.clip.zw - i.pixel) + float2(0.5), float2(0.0), float2(1.0));
    VSOut param = i;
    return shade(param, glyph_atlas, atlas_sampler, _470) * (cover.x * cover.y);
}

fragment vcs_main_out vcs_main(vcs_main_in in [[stage_in]], constant Frame& _470 [[buffer(0)]], texture2d<float> glyph_atlas [[texture(0)]], sampler atlas_sampler [[sampler(0)]], float4 gl_FragCoord [[position]])
{
    vcs_main_out out = {};
    VSOut i;
    i.position = gl_FragCoord;
    i.pixel = in.i_pixel;
    i.uv = in.i_uv;
    i.rect = in.i_rect;
    i.radii = in.i_radii;
    i.color0 = in.i_color0;
    i.color1 = in.i_color1;
    i.border_color = in.i_border_color;
    i.kind = in.i_kind;
    i.params = in.i_params;
    i.data = in.i_data;
    i.clip = in.i_clip;
    VSOut param = i;
    out._entryPointOutput = _main(param, glyph_atlas, atlas_sampler, _470);
    return out;
}

