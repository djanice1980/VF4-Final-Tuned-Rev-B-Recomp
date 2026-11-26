#version 450
// A triangle of the scene (view.h, view_gpu_vertex): the target's pixels and
// z = 1/w. The position's w is 1/z, so the texture coordinates are
// interpolated with the perspective, as the software rasterizer does; the
// colours and z are interpolated on the screen (noperspective), as it does.
//
// A vertex whose a_lit is not 0 was not lit by the machine's thread
// (docs/design.md section 67): its colours are made here, by the same rule
// as view.c's strip_job, from its normal and the point it is lit at (both in
// the game's view space), its strip's record (a_lit - 1) and that record's
// light set.
layout(location = 0) in vec3 a_pos;
layout(location = 1) in vec2 a_uv;
layout(location = 2) in vec4 a_col;
layout(location = 3) in vec4 a_ofs;
layout(location = 4) in uint a_state;
layout(location = 5) in vec3 a_n;
layout(location = 6) in vec3 a_l;
layout(location = 7) in uint a_lit;

// a strip's record, 12 floats: the material's base colour (r, g, b, a,
// 0..255), its specular colour (r, g, b) and power (0: none), the light set,
// 1 when the material takes no lights
layout(std430, set = 0, binding = 0) readonly buffer Lit {
    float lit[];
};
// a light set, 244 floats: the ambient colour (r, g, b, 0), then 15 lights of
// 16: colour (r, g, b), flags (1 on, 2 point, 4 linear fall); direction (x,
// y, z), fall a; position (x, y, z), fall b; cone k, cone b, the diffuse mode,
// the routing
layout(std430, set = 0, binding = 1) readonly buffer Lights {
    float lights[];
};

layout(set = 1, binding = 0) uniform VU {
    vec4 screen;        // target width, height (pixels), depth scale (1 / largest z), 0
} vu;

layout(location = 0) noperspective out vec4 v_col;
layout(location = 1) noperspective out vec4 v_ofs;
layout(location = 2) out vec2 v_uv;
layout(location = 3) noperspective out float v_z;
layout(location = 4) flat out uint v_state;

void light(out vec4 col, out vec4 ofs)
{
    uint r = (a_lit - 1u) * 12u;
    vec4 base = vec4(lit[r], lit[r + 1u], lit[r + 2u], lit[r + 3u]);
    vec3 spec_col = vec3(lit[r + 4u], lit[r + 5u], lit[r + 6u]);
    float power = lit[r + 7u];
    uint s = uint(lit[r + 8u]) * 244u;
    bool unlit = lit[r + 9u] != 0.0;
    vec3 g = a_n;
    vec3 l = vec3(lights[s], lights[s + 1u], lights[s + 2u]);
    vec3 p = vec3(0.0);
    float la = 0.0;
    if (dot(g, g) > 0.0) {
        vec3 e = vec3(0.0);
        if (power > 0.0) {
            float el = length(a_l);
            if (el > 0.0)
                e = vec3(a_l.x, -a_l.y, a_l.z) / el;
        }
        for (uint k = 0u; k < 15u; k++) {
            uint o = s + 4u + k * 16u;
            uint flags = uint(lights[o + 3u]);
            if ((flags & 1u) == 0u)
                continue;
            vec3 lc = vec3(lights[o], lights[o + 1u], lights[o + 2u]);
            vec3 dir = vec3(lights[o + 4u], lights[o + 5u], lights[o + 6u]);
            vec3 t;
            float at = 1.0;
            bool point = (flags & 2u) != 0u;
            if (point) {
                vec3 d = vec3(lights[o + 8u] + a_l.x, lights[o + 9u] - a_l.y, lights[o + 10u] + a_l.z);
                float dist = length(d);
                if (dist <= 0.0)
                    continue;
                t = d / dist;
                float ka = lights[o + 7u], kb = lights[o + 11u];
                at = clamp((flags & 4u) != 0u ? ka * dist + kb : ka / dist + kb, 0.0, 1.0);
            } else {
                t = -dir;
            }
            // a spot light's cone, for a light that goes to the colour as for one that goes to the
            // alpha (section 89)
            float cone = 1.0;
            float ck = lights[o + 12u], cb = lights[o + 13u];
            if (point && ck != 0.0) {
                float cs = -dot(t, dir);
                cone = clamp(ck > 0.0 ? ck * cs + cb : ck * (1.0 - cs) + cb, 0.0, 1.0);
            }
            uint route = uint(lights[o + 15u]);
            if ((route & 6u) == 4u) {
                // a light that goes to the alpha (section 64)
                float amt = lc.r * at;
                if (point && ck != 0.0)
                    amt *= cone;
                if (uint(lights[o + 14u]) != 3u && (route & 1u) == 0u)
                    amt *= max(dot(g, t), 0.0);
                la += (route & 8u) != 0u ? -amt : amt;
                continue;
            }
            at *= cone;
            float d = dot(g, t) * at;
            if (d > 0.0) {
                l += lc * d;
                if (power > 0.0) {
                    vec3 h = t + e;
                    float hl = length(h);
                    if (hl > 0.0) {
                        float cs = dot(g, h) / hl;
                        if (cs > 0.70710678) {
                            float w2 = 2.0 * cs * cs - 1.0;
                            p += lc * (pow(w2 * w2, power) * at);
                        }
                    }
                }
            }
        }
    }
    if (unlit)
        l = vec3(1.0);
    col = vec4(base.rgb * min(l, vec3(1.0)), clamp(base.a + la * 255.0, 0.0, 255.0));
    ofs = vec4(power > 0.0 ? spec_col * min(p, vec3(1.0)) : vec3(0.0), 0.0);
    // whole steps of 255, as the bytes the machine's thread sends
    col = floor(clamp(col, 0.0, 255.0));
    ofs = floor(clamp(ofs, 0.0, 255.0));
}

void main()
{
    float w = a_pos.z > 1e-30 ? 1.0 / a_pos.z : 1.0;
    vec2 ndc = vec2(a_pos.x / vu.screen.x * 2.0 - 1.0, 1.0 - a_pos.y / vu.screen.y * 2.0);
    gl_Position = vec4(ndc * w, clamp(a_pos.z * vu.screen.z, 0.0, 1.0) * w, w);
    if (a_lit != 0u) {
        light(v_col, v_ofs);
    } else {
        v_col = a_col * 255.0;
        v_ofs = a_ofs * 255.0;
    }
    v_uv = a_uv;
    v_z = a_pos.z;
    v_state = a_state;
}
