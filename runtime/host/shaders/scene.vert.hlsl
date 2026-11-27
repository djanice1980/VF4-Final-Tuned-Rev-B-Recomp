// scene.vert's twin in DirectX's shader language, for the GPU renderer through Direct3D 12
// (docs/design.md section 103), and to be kept its twin: the two give the same picture.
//
// A triangle of the scene (view.h, view_gpu_vertex): the target's pixels and z = 1/w. The
// position's w is 1/z, so the texture coordinates and the colors are interpolated with the
// perspective, as the software rasterizer does (docs/design.md section 95: the chip does it for a
// vertex's colors too); z is interpolated on the screen (noperspective), as it does.
//
// A vertex whose a_lit is not 0 was not lit by the machine's thread (docs/design.md section 67):
// its colors are made here, by the same rule as view.c's strip_job, from its normal and the point
// it is lit at (both in the game's view space), its strip's record (a_lit - 1) and that record's
// light set.
//
// SDL's Direct3D 12 back end: a vertex shader's storage buffers are raw buffers in space0, its
// uniform buffer is in space1, and every vertex input's semantic is TEXCOORD with its location.

// a strip's record, 12 floats: the material's base color (r, g, b, a, 0..255), its offset color
// (r, g, b: the highlight's) and the highlight's power (0: none), the light set, 1 when the
// material takes no lights, 1 when its polygons have an offset color
ByteAddressBuffer lit_buf : register(t0, space0);
// a light set, 244 floats: the ambient color (r, g, b), 1 when light over full goes into the
// offset color (sections 93 and 98), then 15 lights of 16: color (r, g, b), flags (1 on, 2 point,
// 4 linear fall); direction (x, y, z), fall a; position (x, y, z), fall b; cone k, cone b, the
// diffuse mode, the routing
ByteAddressBuffer lights_buf : register(t1, space0);

cbuffer VU : register(b0, space1) {
    float4 vu_screen;       // target width, height (pixels), depth scale (1 / largest z), 0
};

struct VIn {
    float3 a_pos : TEXCOORD0;
    float2 a_uv : TEXCOORD1;
    float4 a_col : TEXCOORD2;
    float4 a_ofs : TEXCOORD3;
    uint a_state : TEXCOORD4;
    float3 a_n : TEXCOORD5;
    float3 a_l : TEXCOORD6;
    uint a_lit : TEXCOORD7;
};

struct VOut {
    float4 v_col : TEXCOORD0;
    float4 v_ofs : TEXCOORD1;
    float2 v_uv : TEXCOORD2;
    noperspective float v_z : TEXCOORD3;
    nointerpolation uint v_state : TEXCOORD4;
    // precise: the position's sum is done as written, step by step, as the twin's is (DirectX's
    // compiler may otherwise join a product and a sum, and an edge then falls a pixel aside)
    precise float4 pos : SV_Position;
};

float lit(uint i)
{
    return asfloat(lit_buf.Load(i * 4u));
}

float lights(uint i)
{
    return asfloat(lights_buf.Load(i * 4u));
}

void light(VIn v, out float4 col, out float4 ofs)
{
    uint r = (v.a_lit - 1u) * 12u;
    float4 base = float4(lit(r), lit(r + 1u), lit(r + 2u), lit(r + 3u));
    float3 spec_col = float3(lit(r + 4u), lit(r + 5u), lit(r + 6u));
    float gloss = lit(r + 7u);
    uint s = uint(lit(r + 8u)) * 244u;
    bool unlit = lit(r + 9u) != 0.0;
    bool use_ofs = lit(r + 10u) != 0.0;     // the polygon has an offset color (section 93)
    float3 g = v.a_n;
    float3 l = float3(lights(s), lights(s + 1u), lights(s + 2u));
    bool over = lights(s + 3u) != 0.0;      // what is over full goes into the offset color
    float3 q = float3(0, 0, 0);             // the highlight, which goes into the base color
    float la = 0.0;
    if (dot(g, g) > 0.0) {
        float3 e = float3(0, 0, 0);
        if (gloss > 0.0) {
            float el = length(v.a_l);
            if (el > 0.0)
                e = float3(v.a_l.x, -v.a_l.y, v.a_l.z) / el;
        }
        for (uint k = 0u; k < 15u; k++) {
            uint o = s + 4u + k * 16u;
            uint flags = uint(lights(o + 3u));
            if ((flags & 1u) == 0u)
                continue;
            float3 lc = float3(lights(o), lights(o + 1u), lights(o + 2u));
            float3 dir = float3(lights(o + 4u), lights(o + 5u), lights(o + 6u));
            float3 t;
            float at = 1.0;
            bool point_light = (flags & 2u) != 0u;
            if (point_light) {
                float3 d = float3(lights(o + 8u) + v.a_l.x, lights(o + 9u) - v.a_l.y, lights(o + 10u) + v.a_l.z);
                float dist = length(d);
                if (dist <= 0.0)
                    continue;
                t = d / dist;
                float ka = lights(o + 7u), kb = lights(o + 11u);
                at = clamp((flags & 4u) != 0u ? ka * dist + kb : ka / dist + kb, 0.0, 1.0);
            } else {
                t = -dir;
            }
            // a spot light's cone, for a light that goes to the color as for one that goes to the
            // alpha (section 89)
            float cone = 1.0;
            float ck = lights(o + 12u), cb = lights(o + 13u);
            if (point_light && ck != 0.0) {
                float cs = -dot(t, dir);
                cone = clamp(ck > 0.0 ? ck * cs + cb : ck * (1.0 - cs) + cb, 0.0, 1.0);
            }
            uint route = uint(lights(o + 15u));
            if ((route & 6u) == 4u) {
                // a light that goes to the alpha (section 64)
                float amt = lc.r * at;
                if (point_light && ck != 0.0)
                    amt *= cone;
                if (uint(lights(o + 14u)) != 3u && (route & 1u) == 0u)
                    amt *= max(dot(g, t), 0.0);
                la += (route & 8u) != 0u ? -amt : amt;
                continue;
            }
            at *= cone;
            float d = dot(g, t) * at;
            if (d > 0.0)
                l += lc * d;
            if (gloss > 0.0) {
                // the eye's direction mirrored at the surface, against the way to the light, to
                // the gloss's power; whichever side of the surface the light is on (section 98)
                float ne = dot(g, e);
                float rl = dot(2.0 * ne * g - e, t);
                if (rl > 0.0)
                    q += lc * (pow(min(rl, 1.0), gloss) * at);
            }
        }
    }
    // the product is held to full, not the lights' sum; what is over goes into the offset color
    // where the light set asks for it and the polygon has one (sections 93 and 98)
    float3 lit_rgb = unlit ? base.rgb : base.rgb * l + spec_col * q;
    float3 extra = over && !unlit && use_ofs ? max(lit_rgb - float3(255.0, 255.0, 255.0), float3(0, 0, 0)) : float3(0, 0, 0);
    col = float4(lit_rgb, clamp(base.a + la * 255.0, 0.0, 255.0));
    ofs = float4(extra, 0.0);
    // whole steps of 255, to the nearest, as the bytes the machine's thread sends (section 95)
    col = floor(clamp(col, 0.0, 255.0) + 0.5);
    ofs = floor(clamp(ofs, 0.0, 255.0) + 0.5);
}

VOut main(VIn v)
{
    VOut o;
    float w = v.a_pos.z > 1e-30 ? 1.0 / v.a_pos.z : 1.0;
    float2 ndc = float2(v.a_pos.x / vu_screen.x * 2.0 - 1.0, 1.0 - v.a_pos.y / vu_screen.y * 2.0);
    o.pos = float4(ndc * w, clamp(v.a_pos.z * vu_screen.z, 0.0, 1.0) * w, w);
    if (v.a_lit != 0u) {
        light(v, o.v_col, o.v_ofs);
    } else {
        o.v_col = v.a_col * 255.0;
        o.v_ofs = v.a_ofs * 255.0;
    }
    o.v_uv = v.a_uv;
    o.v_z = v.a_pos.z;
    o.v_state = v.a_state;
    return o;
}
