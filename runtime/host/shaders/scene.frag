#version 450
// A pixel of a scene's triangle, by the software rasterizer's rules
// (runtime/machine/view.c, raster()): the user clip, the texture (its own
// wrap, mirror and clamp, nearest or four texels, the mip-map level, bump
// maps), the shading instruction, the offset color, the alpha test of the
// punch-through list, the fog of the table or of the vertex. The colors keep
// their fractions through the sum and are rounded once, by the picture's own
// 8 bits (docs/design.md section 95).
layout(location = 0) in vec4 v_col;
layout(location = 1) in vec4 v_ofs;
layout(location = 2) in vec2 v_uv;
layout(location = 3) noperspective in float v_z;
layout(location = 4) flat in uint v_state;
layout(location = 0) out vec4 o_col;

struct State {
    uint pcw, isp, tsp, tcw;
    uint tex_at, tex_w, tex_h, tex_levels;
    ivec4 clip;
    uvec4 pad;          // x: 1 + the target picture the texture is (0: the arena's); y: its columns beyond each side
};
// the pictures of the passes into textures (docs/design.md section 78: a stage's reflection in
// its water), drawn before this pass
layout(set = 2, binding = 0) uniform sampler2D t_pass0;
layout(set = 2, binding = 1) uniform sampler2D t_pass1;
layout(set = 2, binding = 2) uniform sampler2D t_pass2;
layout(set = 2, binding = 3) uniform sampler2D t_pass3;
layout(std430, set = 2, binding = 4) readonly buffer States { State st[]; };
layout(std430, set = 2, binding = 5) readonly buffer Texels { uint tx[]; };

layout(set = 3, binding = 0) uniform FU {
    vec4 p0;            // render scale, alpha reference, fog density, list (0 opaque, 1 punch-through, 2 translucent)
    uvec4 p1;           // fog on, fog color (table), fog color (vertex), 0
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

// which target picture the texture is (0: none), and how far past each side its u may go
uint pass_k;
int pass_ox;

// u: as tex_at, but where a target picture has columns beyond the texture's sides (the wide screen)
// a clamped coordinate may go that far
int tex_at_u(int t, int size, bool clampit, bool flip)
{
    if (pass_k != 0u && clampit)
        return clamp(t, -pass_ox, size - 1 + pass_ox);
    return tex_at(t, size, clampit, flip);
}

vec4 texel(uint at, int w, int x, int y)
{
    if (pass_k != 0u) {
        // a pixel of the pass, as texture memory would hold it and the texture's decoder give it
        // back (view.c: target_store's 5, 6 and 5 bits, texel16's widening), opaque
        ivec2 p = ivec2(x + pass_ox, y);
        vec3 f = pass_k == 1u ? texelFetch(t_pass0, p, 0).rgb : pass_k == 2u ? texelFetch(t_pass1, p, 0).rgb
               : pass_k == 3u ? texelFetch(t_pass2, p, 0).rgb : texelFetch(t_pass3, p, 0).rgb;
        ivec3 q = ivec3(f * 255.0 + 0.5);
        q = ivec3(q.r >> 3, q.g >> 2, q.b >> 3);
        return vec4(float(q.r << 3 | q.r >> 2), float(q.g << 2 | q.g >> 4), float(q.b << 3 | q.b >> 2), 255.0);
    }
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
    // the vertex's colors with 8 more bits below the point, as raster() carries them
    vec4 vc = clamp(floor(v_col * 256.0 + 0.5) / 256.0, 0.0, 255.0);
    vec3 c = vc.rgb;
    float ia = (tsp >> 20 & 1u) != 0u ? vc.a : 255.0;
    bool textured = s.tex_w != 0u;
    bool bump = textured && (s.tcw >> 27 & 7u) == 4u;
    bool has_ofs = (s.pcw >> 2 & 1u) != 0u && textured;
    vec4 ofs = clamp(floor(v_ofs * 256.0 + 0.5) / 256.0, 0.0, 255.0);
    pass_k = s.pad.x;
    pass_ox = int(s.pad.y);
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
            int x0 = tex_at_u(ix - 16384, tw, clamp_u, flip_u), x1 = tex_at_u(ix - 16383, tw, clamp_u, flip_u);
            int y0 = tex_at(iy - 16384, th, clamp_v, flip_v), y1 = tex_at(iy - 16383, th, clamp_v, flip_v);
            vec4 top = mix(texel(at, tw, x0, y0), texel(at, tw, x1, y0), wx);
            vec4 bot = mix(texel(at, tw, x0, y1), texel(at, tw, x1, y1), wx);
            t = mix(top, bot, wy);
        } else {
            int ix = int(v_uv.x * float(tw) + 16384.0) - 16384, iy = int(v_uv.y * float(th) + 16384.0) - 16384;
            t = texel(at, tw, tex_at_u(ix, tw, clamp_u, flip_u), tex_at(iy, th, clamp_v, flip_v));
        }
        if (bump) {
            // the texel is two angles, the offset color the light: K1 + K2 sin S + K3 cos S cos (R - Q)
            float s_ang = t.g * (PI / 2.0 / 256.0), r_ang = t.b * (2.0 * PI / 256.0);
            float k1 = ofs.a / 255.0, k2 = ofs.r / 255.0, k3 = ofs.g / 255.0, q = ofs.b * (2.0 * PI / 256.0);
            float v = clamp((k1 + k2 * sin(s_ang) + k3 * cos(s_ang) * cos(r_ang - q)) * 255.0, 0.0, 255.0);
            t = vec4(v);
        }
        float ta = (tsp >> 19 & 1u) != 0u ? 255.0 : t.a;
        uint shade = tsp >> 6 & 3u;
        if ((shade & 1u) != 0u) {
            c = t.rgb * c / 255.0;
            ia = shade == 1u ? ta : ta * ia / 255.0;
        } else if (shade == 0u) {
            c = t.rgb;
            ia = ta;
        } else {
            float k = ta / 255.0;
            c = t.rgb * k + c * (1.0 - k);
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
            c = c + (fc - c) * f / 256.0;
        }
    } else if (fog_mode == 1u && has_ofs) {
        float f = floor(ofs.a);
        f = f >= 255.0 ? 256.0 : f + floor(f / 128.0);
        c = c + (rgb_of(fu.p1.z) - c) * f / 256.0;
    }
    o_col = vec4(c, ia) / 255.0;
}
