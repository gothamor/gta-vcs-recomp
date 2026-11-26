#pragma clang diagnostic ignored "-Wmissing-prototypes"

#include <metal_stdlib>
#include <simd/simd.h>

using namespace metal;

struct Instance
{
    float4 rect;
    float4 uv;
    float4 radii;
    float4 clip;
    float4 color0;
    float4 color1;
    float4 border_color;
    uint kind;
    float4 params;
};

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
    float2 _entryPointOutput_pixel [[user(locn0)]];
    float2 _entryPointOutput_uv [[user(locn1)]];
    float4 _entryPointOutput_rect [[user(locn2)]];
    float4 _entryPointOutput_radii [[user(locn3)]];
    float4 _entryPointOutput_color0 [[user(locn4)]];
    float4 _entryPointOutput_color1 [[user(locn5)]];
    float4 _entryPointOutput_border_color [[user(locn6)]];
    uint _entryPointOutput_kind [[user(locn7)]];
    float4 _entryPointOutput_params [[user(locn8)]];
    float4 _entryPointOutput_data [[user(locn9)]];
    float4 _entryPointOutput_clip [[user(locn10)]];
    float4 gl_Position [[position]];
};

struct vcs_main_in
{
    float4 inst_rect [[attribute(0)]];
    float4 inst_uv [[attribute(1)]];
    float4 inst_radii [[attribute(2)]];
    float4 inst_clip [[attribute(3)]];
    float4 inst_color0 [[attribute(4)]];
    float4 inst_color1 [[attribute(5)]];
    float4 inst_border_color [[attribute(6)]];
    uint inst_kind [[attribute(7)]];
    float4 inst_params [[attribute(8)]];
};

static inline __attribute__((always_inline))
VSOut _main(thread const uint& vertex_id, thread const Instance& inst, constant Frame& _119)
{
    float2 grow = float2(1.0);
    if (((inst.kind == 1u) || (inst.kind == 3u)) || (inst.kind == 4u))
    {
        grow = float2(0.0);
    }
    if (inst.kind == 2u)
    {
        grow = float2((3.0 * inst.params.y) + 1.0);
    }
    if ((inst.kind == 1u) && (inst.params.y > 0.0))
    {
        grow = float2(inst.params.y + 1.0);
    }
    float2 lo = fast::max(inst.rect.xy - grow, floor(inst.clip.xy));
    float2 hi = fast::min(inst.rect.zw + grow, ceil(inst.clip.zw));
    hi = fast::max(hi, lo);
    float2 corner = float2(float(vertex_id & 1u), float(vertex_id >> uint(1)));
    float2 p = mix(lo, hi, corner);
    VSOut o;
    o.position = float4(((p * (float2(2.0) / _119.viewport_size)) * float2(1.0, -1.0)) + float2(-1.0, 1.0), 0.0, 1.0);
    o.pixel = p;
    float2 t = (p - inst.rect.xy) / fast::max(inst.rect.zw - inst.rect.xy, float2(9.9999997473787516355514526367188e-06));
    o.uv = mix(inst.uv.xy, inst.uv.zw, t) * _119.atlas_inv_size;
    o.rect = inst.rect;
    o.radii = inst.radii;
    o.color0 = inst.color0;
    o.color1 = inst.color1;
    o.border_color = inst.border_color;
    o.kind = inst.kind;
    o.params = inst.params;
    o.data = inst.uv;
    o.clip = inst.clip;
    return o;
}

vertex vcs_main_out vcs_main(vcs_main_in in [[stage_in]], constant Frame& _119 [[buffer(0)]], uint gl_VertexIndex [[vertex_id]])
{
    vcs_main_out out = {};
    uint vertex_id = gl_VertexIndex;
    Instance inst;
    inst.rect = in.inst_rect;
    inst.uv = in.inst_uv;
    inst.radii = in.inst_radii;
    inst.clip = in.inst_clip;
    inst.color0 = in.inst_color0;
    inst.color1 = in.inst_color1;
    inst.border_color = in.inst_border_color;
    inst.kind = in.inst_kind;
    inst.params = in.inst_params;
    uint param = vertex_id;
    Instance param_1 = inst;
    VSOut flattenTemp = _main(param, param_1, _119);
    out.gl_Position = flattenTemp.position;
    out._entryPointOutput_pixel = flattenTemp.pixel;
    out._entryPointOutput_uv = flattenTemp.uv;
    out._entryPointOutput_rect = flattenTemp.rect;
    out._entryPointOutput_radii = flattenTemp.radii;
    out._entryPointOutput_color0 = flattenTemp.color0;
    out._entryPointOutput_color1 = flattenTemp.color1;
    out._entryPointOutput_border_color = flattenTemp.border_color;
    out._entryPointOutput_kind = flattenTemp.kind;
    out._entryPointOutput_params = flattenTemp.params;
    out._entryPointOutput_data = flattenTemp.data;
    out._entryPointOutput_clip = flattenTemp.clip;
    return out;
}

