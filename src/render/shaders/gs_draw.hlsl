// GS primitive drawing for the HW GS renderer (gs_hw.cpp): HLSL port of its former GLSL shaders.
// Render targets hold PS2 rows top-down (texture row = PS2 row); colours and alpha are 0..255 values / 255.
// DUAL_SOURCE=1 also outputs the blend factor C as the second colour (Src1Alpha blending).

cbuffer Draw : register(b0)
{
    float4 targetRtFix;    // target size (PS2 pixels), render-target texel scale, FIX
    int4 tmeTfxTccFst;
    int4 linearIipFgeAte;
    int4 atstArefAfailRt;  // ATST, AREF, pass (0: alpha-test passing pixels, 1: failing ones), texture is a render target
    int4 texSizeWrap;      // texture w, h, WMS, WMT
    int4 region;           // MINU, MAXU, MINV, MAXV
    float4 rtOrigin;       // xy: texture origin inside the render target (PS2 pixels)
    int4 blendCPremul;     // C (0 As, 1 Ad, 2 FIX), premultiply (0 none, 1 rgb *= C, 2 rgb *= 1 + C)
    float4 fogColor;       // rgb 0..255
};

struct VSIn
{
    float3 pos : POSITION;  // PS2 pixels; depth 0..1
    float3 stq : TEXCOORD0; // STQ, or U/V texels with q = 1 (FST)
    float4 col : COLOR0;
    float fog : TEXCOORD1;  // 0..1 (1 = no fog)
};

struct VSOut
{
    float4 pos : SV_Position;
    noperspective float3 stq : TEXCOORD0;
    noperspective float4 col : COLOR0;
    nointerpolation float4 colFlat : COLOR1; // the recorder gives flat primitives one colour on every vertex
    noperspective float fog : TEXCOORD1;
    noperspective float depth : TEXCOORD2;
};

VSOut vs_main(VSIn i)
{
    VSOut o;
    const float2 p = i.pos.xy / targetRtFix.xy;
    o.pos = float4(p.x * 2.0 - 1.0, 1.0 - p.y * 2.0, i.pos.z, 1.0);
    o.depth = i.pos.z;
    o.stq = i.stq;
    o.col = i.col;
    o.colFlat = i.col;
    o.fog = i.fog;
    return o;
}

Texture2D<float4> tex : register(t0);

int wrapc(int c, int size, int mode, int mn, int mx)
{
    int r = (c & mn) | mx;                       // REGION_REPEAT
    if (mode == 0) r = c & (size - 1);           // REPEAT
    else if (mode == 1) r = clamp(c, 0, size - 1); // CLAMP
    else if (mode == 2) r = clamp(c, mn, mx);    // REGION_CLAMP
    return r;
}

float4 fetchTexel(int2 t)
{
    t.x = wrapc(t.x, texSizeWrap.x, texSizeWrap.z, region.x, region.y);
    t.y = wrapc(t.y, texSizeWrap.y, texSizeWrap.w, region.z, region.w);
    int2 at = t;
    if (atstArefAfailRt.w == 1)
        at = int2((float2(t) + rtOrigin.xy + 0.5) * targetRtFix.z);
    return tex.Load(int3(at, 0)) * 255.0;
}

float4 sampleTex(float2 uv)
{
    float4 r;
    [branch] if (linearIipFgeAte.x == 0)
    {
        r = fetchTexel(int2(floor(uv)));
    }
    else
    {
        const float2 p = uv - 0.5;
        const int2 i = int2(floor(p));
        const float2 f = p - float2(i);
        const float4 c00 = fetchTexel(i), c10 = fetchTexel(i + int2(1, 0));
        const float4 c01 = fetchTexel(i + int2(0, 1)), c11 = fetchTexel(i + int2(1, 1));
        r = lerp(lerp(c00, c10, f.x), lerp(c01, c11, f.x), f.y);
    }
    return r;
}

struct PSOut
{
    float4 color : SV_Target0;
#if DUAL_SOURCE
    float4 blend : SV_Target1;
#endif
    float depth : SV_Depth;
};

PSOut ps_main(VSOut i)
{
    PSOut o;
    // GS Z is Z24/Z32 / 2^n: written directly (going through NDC rounded near values and collapsed far ones)
    o.depth = i.depth;
    const float4 vc = floor((linearIipFgeAte.y == 1 ? i.col : i.colFlat) * 255.0 + 0.5);
    float4 c = vc;
    if (tmeTfxTccFst.x == 1)
    {
        const float2 uv = (tmeTfxTccFst.w == 1) ? i.stq.xy : (i.stq.xy / i.stq.z) * float2(texSizeWrap.xy);
        const float4 t = sampleTex(uv);
        const int tfx = tmeTfxTccFst.y;
        const bool tcc = tmeTfxTccFst.z == 1;
        if (tfx == 0) { c.rgb = t.rgb * vc.rgb / 128.0; c.a = tcc ? t.a * vc.a / 128.0 : vc.a; }
        else if (tfx == 1) { c.rgb = t.rgb; c.a = tcc ? t.a : vc.a; }
        else if (tfx == 2) { c.rgb = t.rgb * vc.rgb / 128.0 + vc.a; c.a = tcc ? t.a + vc.a : vc.a; }
        else { c.rgb = t.rgb * vc.rgb / 128.0 + vc.a; c.a = tcc ? t.a : vc.a; }
    }
    c = clamp(floor(c), 0.0, 255.0);
    if (linearIipFgeAte.z == 1)
    {
        const float f = floor(i.fog * 255.0 + 0.5);
        c.rgb = floor((c.rgb * f + fogColor.rgb * (255.0 - f)) / 255.0);
    }
    bool passed = true;
    if (linearIipFgeAte.w == 1)
    {
        const float a = c.a, r = float(atstArefAfailRt.y);
        const int atst = atstArefAfailRt.x;
        if (atst == 0) passed = false;
        else if (atst == 2) passed = a < r;
        else if (atst == 3) passed = a <= r;
        else if (atst == 4) passed = a == r;
        else if (atst == 5) passed = a >= r;
        else if (atst == 6) passed = a > r;
        else if (atst == 7) passed = a != r;
    }
    if (atstArefAfailRt.z == 0 && !passed) discard;
    if (atstArefAfailRt.z == 1 && passed) discard;
    const int bc = blendCPremul.x;
    const float cf = (bc == 0) ? c.a / 128.0 : ((bc == 2) ? targetRtFix.w / 128.0 : 1.0);
    float3 rgb = c.rgb;
    if (blendCPremul.y == 1) rgb *= cf;
    else if (blendCPremul.y == 2) rgb *= (1.0 + cf);
    o.color = float4(rgb / 255.0, c.a / 255.0);
#if DUAL_SOURCE
    o.blend = min(cf, 1.0).xxxx;
#endif
    return o;
}
