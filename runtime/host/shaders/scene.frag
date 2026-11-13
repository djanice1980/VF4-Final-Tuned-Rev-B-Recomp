#version 450
// A pixel of a scene's triangle, by the software rasterizer's rules
// (runtime/machine/view.c, raster()): the user clip, the texture (its own
// wrap, mirror and clamp, nearest or four texels, the mip-map level, bump
// maps), the shading instruction, the offset colour, the alpha test of the
// punch-through list, the fog of the table or of the vertex.
layout(location = 0) noperspective in vec4 v_col;
layout(location = 1) noperspective in vec4 v_ofs;
layout(location = 2) in vec2 v_uv;
layout(location = 3) noperspective in float v_z;
layout(location = 4) flat in uint v_state;
layout(location = 0) out vec4 o_col;

struct State {
    uint pcw, isp, tsp, tcw;
    uint tex_at, tex_w, tex_h, tex_levels;
    ivec4 clip;
    uvec4 pad;
};
layout(std430, set = 2, binding = 0) readonly buffer States { State st[]; };
layout(std430, set = 2, binding = 1) readonly buffer Texels { uint tx[]; };

layout(set = 3, binding = 0) uniform FU {
    vec4 p0;            // render scale, alpha reference, fog density, list (0 opaque, 1 punch-through, 2 translucent)
    uvec4 p1;           // fog on, fog colour (table), fog colour (vertex), 0
    uvec4 fog[33];      // the fog table, 129 steps of 0..255
} fu;

const float PI = 3.14159265358979;

int tex_at(int t, int size, bool clampit, bool flip)
{
    if (clampit)
        return clamp(t, 0, size - 1);
    if (flip && (t & size) != 0)
        return ~t & (size - 1);
    return t & (size - 1);
}

vec4 texel(uint at, int w, int x, int y)
{
    uint c = tx[at + uint(y * w + x)];
    return vec4(float(c >> 16 & 255u), float(c >> 8 & 255u), float(c & 255u), float(c >> 24));
}

uint fog_step(uint i)
{
    return fu.fog[i >> 2][i & 3u];
}

float fog_at(float z)
{
    float s = z * fu.p0.z;
    if (!(s > 1.0))
        s = 1.0;
    else if (s > 255.9999)
        s = 255.9999;
    uint b = floatBitsToUint(s);
    uint i = (b >> 19) - (127u << 4), fr = b >> 11 & 255u;
    return float((fog_step(i) * (256u - fr) + fog_step(i + 1u) * fr) >> 8);
}

vec3 rgb_of(uint c)
{
    return vec3(float(c >> 16 & 255u), float(c >> 8 & 255u), float(c & 255u));
}

void main()
{
    State s = st[v_state];
    float scale = fu.p0.x;
    uint list = uint(fu.p0.w);
    // the derivatives first, where every pixel of the quad still runs
    vec2 dx = dFdx(v_uv), dy = dFdy(v_uv);

    uint clip_mode = s.pcw >> 16 & 3u;
    if (clip_mode >= 2u) {
        ivec2 p = ivec2(floor(gl_FragCoord.xy / scale));
        bool inside = p.x >= s.clip.x && p.x <= s.clip.z && p.y >= s.clip.y && p.y <= s.clip.w;
        if (inside != (clip_mode == 2u))
            discard;
    }
    uint tsp = s.tsp;
    vec3 c = clamp(floor(v_col.rgb), 0.0, 255.0);
    float ia = (tsp >> 20 & 1u) != 0u ? clamp(floor(v_col.a), 0.0, 255.0) : 255.0;
    bool textured = s.tex_w != 0u;
    bool bump = textured && (s.tcw >> 27 & 7u) == 4u;
    bool has_ofs = (s.pcw >> 2 & 1u) != 0u && textured;
    vec4 ofs = clamp(floor(v_ofs), 0.0, 255.0);
    if (textured) {
        int tw0 = int(s.tex_w), th0 = int(s.tex_h);
        bool clamp_u = (tsp >> 16 & 1u) != 0u, clamp_v = (tsp >> 15 & 1u) != 0u;
        bool flip_u = (tsp >> 18 & 1u) != 0u, flip_v = (tsp >> 17 & 1u) != 0u;
        // the mip-map level: by how many texels a pixel covers, times the polygon's adjustment
        int lv = 0;
        float dadj = float(tsp >> 8 & 15u) * 0.25;
        int maxl = dadj == 0.0 ? 0 : int(s.tex_levels) - 1;
        if (maxl > 0) {
            vec2 sz = vec2(tw0, th0);
            vec2 ax = dx * sz, ay = dy * sz;
            float r2 = max(dot(ax, ax), dot(ay, ay)) * dadj * dadj;
            if (r2 > 2.0)
                lv = min(int((floatBitsToUint(r2) >> 23) - 126u) >> 1, maxl);
        }
        uint at = s.tex_at;
        for (int l = 0; l < lv; l++)
            at += uint((tw0 >> l) * (th0 >> l));
        int tw = tw0 >> lv, th = th0 >> lv;
        vec4 t;
        if ((tsp >> 13 & 3u) != 0u && !bump) {
            float fu_ = v_uv.x * float(tw) + 16383.5, fv = v_uv.y * float(th) + 16383.5;
            int ix = int(fu_), iy = int(fv);
            float wx = floor((fu_ - float(ix)) * 256.0) / 256.0, wy = floor((fv - float(iy)) * 256.0) / 256.0;
            int x0 = tex_at(ix - 16384, tw, clamp_u, flip_u), x1 = tex_at(ix - 16383, tw, clamp_u, flip_u);
            int y0 = tex_at(iy - 16384, th, clamp_v, flip_v), y1 = tex_at(iy - 16383, th, clamp_v, flip_v);
            vec4 top = mix(texel(at, tw, x0, y0), texel(at, tw, x1, y0), wx);
            vec4 bot = mix(texel(at, tw, x0, y1), texel(at, tw, x1, y1), wx);
            t = floor(mix(top, bot, wy));
        } else {
            int ix = int(v_uv.x * float(tw) + 16384.0) - 16384, iy = int(v_uv.y * float(th) + 16384.0) - 16384;
            t = texel(at, tw, tex_at(ix, tw, clamp_u, flip_u), tex_at(iy, th, clamp_v, flip_v));
        }
        if (bump) {
            // the texel is two angles, the offset colour the light: K1 + K2 sin S + K3 cos S cos (R - Q)
            float s_ang = t.g * (PI / 2.0 / 256.0), r_ang = t.b * (2.0 * PI / 256.0);
            float k1 = ofs.a / 255.0, k2 = ofs.r / 255.0, k3 = ofs.g / 255.0, q = ofs.b * (2.0 * PI / 256.0);
            float v = clamp(floor((k1 + k2 * sin(s_ang) + k3 * cos(s_ang) * cos(r_ang - q)) * 255.0), 0.0, 255.0);
            t = vec4(v);
        }
        float ta = (tsp >> 19 & 1u) != 0u ? 255.0 : t.a;
        uint shade = tsp >> 6 & 3u;
        if ((shade & 1u) != 0u) {
            c = floor(t.rgb * c / 255.0);
            ia = shade == 1u ? ta : floor(ta * ia / 255.0);
        } else if (shade == 0u) {
            c = t.rgb;
            ia = ta;
        } else {
            float k = ta / 255.0;
            c = floor(t.rgb * k + c * (1.0 - k));
        }
        if (has_ofs && !bump)
            c = min(c + ofs.rgb, vec3(255.0));
    }
    if (list == 1u && ia < fu.p0.y)
        discard;
    uint fog_mode = tsp >> 22 & 3u;
    if (fu.p1.x != 0u && (fog_mode == 0u || fog_mode == 3u)) {
        float f = fog_at(v_z);
        vec3 fc = rgb_of(fu.p1.y);
        if (fog_mode == 3u) {
            c = fc;
            ia = f;
        } else if (f > 0.0) {
            f += floor(f / 128.0);
            c = c + floor((fc - c) * f / 256.0);
        }
    } else if (fog_mode == 1u && has_ofs) {
        float f = ofs.a;
        f = f >= 255.0 ? 256.0 : f + floor(f / 128.0);
        c = c + floor((rgb_of(fu.p1.z) - c) * f / 256.0);
    }
    o_col = vec4(c, ia) / 255.0;
}
