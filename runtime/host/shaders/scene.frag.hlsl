// scene.frag's twin in DirectX's shader language, for the GPU renderer through Direct3D 12
// (docs/design.md section 103), and to be kept its twin: the two give the same picture.
//
// A pixel of a scene's triangle, by the software rasterizer's rules (runtime/machine/view.c,
// raster()): the user clip, the texture (its own wrap, mirror and clamp, nearest or four texels,
// the mip-map level, bump maps), the shading instruction, the offset color, the alpha test of the
// punch-through list, the fog of the table or of the vertex. The colors keep their fractions
// through the sum and are rounded once, by the picture's own 8 bits (docs/design.md section 95).
//
// SDL's Direct3D 12 back end: a pixel shader's sampled textures come first in space2 (with their
// samplers), then its storage buffers, which are raw buffers; its uniform buffer is in space3.

struct PIn {
    float4 v_col : TEXCOORD0;
    float4 v_ofs : TEXCOORD1;
    float2 v_uv : TEXCOORD2;
    noperspective float v_z : TEXCOORD3;
    nointerpolation uint v_state : TEXCOORD4;
    float4 pos : SV_Position;
};

// the pictures of the passes into textures (docs/design.md section 78: a stage's reflection in
// its water), drawn before this pass
Texture2D<float4> t_pass0 : register(t0, space2);
Texture2D<float4> t_pass1 : register(t1, space2);
Texture2D<float4> t_pass2 : register(t2, space2);
Texture2D<float4> t_pass3 : register(t3, space2);
SamplerState s_pass0 : register(s0, space2);
SamplerState s_pass1 : register(s1, space2);
SamplerState s_pass2 : register(s2, space2);
SamplerState s_pass3 : register(s3, space2);
// a state, 16 words: pcw, isp, tsp, tcw; tex_at, tex_w, tex_h, tex_levels; the clip (4); pad (x: 1
// + the target picture the texture is (0: the arena's); y: its columns beyond each side)
ByteAddressBuffer st_buf : register(t4, space2);
ByteAddressBuffer tx_buf : register(t5, space2);

cbuffer FU : register(b0, space3) {
    float4 fu_p0;           // render scale, alpha reference, fog density, list (0 opaque, 1 punch-through, 2 translucent)
    uint4 fu_p1;            // fog on, fog color (table), fog color (vertex), 0
    uint4 fu_fog[33];       // the fog table, 129 steps of 0..255
};

static const float PI = 3.14159265358979;

int tex_at(int t, int size, bool clampit, bool flip)
{
    if (clampit)
        return clamp(t, 0, size - 1);
    if (flip && (t & size) != 0)
        return ~t & (size - 1);
    return t & (size - 1);
}

// which target picture the texture is (0: none), and how far past each side its u may go
static uint pass_k;
static int pass_ox;

// u: as tex_at, but where a target picture has columns beyond the texture's sides (the wide screen)
// a clamped coordinate may go that far
int tex_at_u(int t, int size, bool clampit, bool flip)
{
    if (pass_k != 0u && clampit)
        return clamp(t, -pass_ox, size - 1 + pass_ox);
    return tex_at(t, size, clampit, flip);
}

float4 texel(uint at, int w, int x, int y)
{
    if (pass_k != 0u) {
        // a pixel of the pass, as texture memory would hold it and the texture's decoder give it
        // back (view.c: target_store's 5, 6 and 5 bits, texel16's widening), opaque
        int3 p = int3(x + pass_ox, y, 0);
        float3 f;
        if (pass_k == 1u)
            f = t_pass0.Load(p).rgb;
        else if (pass_k == 2u)
            f = t_pass1.Load(p).rgb;
        else if (pass_k == 3u)
            f = t_pass2.Load(p).rgb;
        else
            f = t_pass3.Load(p).rgb;
        int3 q = int3(f * 255.0 + 0.5);
        q = int3(q.r >> 3, q.g >> 2, q.b >> 3);
        return float4(float(q.r << 3 | q.r >> 2), float(q.g << 2 | q.g >> 4), float(q.b << 3 | q.b >> 2), 255.0);
    }
    uint c = tx_buf.Load((at + uint(y * w + x)) * 4u);
    return float4(float(c >> 16 & 255u), float(c >> 8 & 255u), float(c & 255u), float(c >> 24));
}

// (a vector of the constants is not indexed by a variable: DXC fails on it for DXIL)
uint fog_step(uint i)
{
    uint4 v = fu_fog[i >> 2];
    uint k = i & 3u;
    return k == 0u ? v.x : k == 1u ? v.y : k == 2u ? v.z : v.w;
}

float fog_at(float z)
{
    float s = z * fu_p0.z;
    if (!(s > 1.0))
        s = 1.0;
    else if (s > 255.9999)
        s = 255.9999;
    uint b = asuint(s);
    uint i = (b >> 19) - (127u << 4), fr = b >> 11 & 255u;
    return float((fog_step(i) * (256u - fr) + fog_step(i + 1u) * fr) >> 8);
}

float3 rgb_of(uint c)
{
    return float3(float(c >> 16 & 255u), float(c >> 8 & 255u), float(c & 255u));
}

float4 main(PIn v) : SV_Target0
{
    uint sb = v.v_state * 64u;
    uint4 s_w = st_buf.Load4(sb);           // pcw, isp, tsp, tcw
    uint4 s_tex = st_buf.Load4(sb + 16u);   // tex_at, tex_w, tex_h, tex_levels
    int4 s_clip = int4(st_buf.Load4(sb + 32u));
    uint4 s_pad = st_buf.Load4(sb + 48u);
    float scale = fu_p0.x;
    uint list = uint(fu_p0.w);
    // the derivatives first, where every pixel of the quad still runs. The fine kind by name, as
    // the twin: DirectX's plain ddx is the coarse kind (one difference for a block of four
    // pixels), and a texture's level then changed a pixel apart from Vulkan's
    float2 dx = ddx_fine(v.v_uv), dy = ddy_fine(v.v_uv);

    uint clip_mode = s_w.x >> 16 & 3u;
    if (clip_mode >= 2u) {
        int2 p = int2(floor(v.pos.xy / scale));
        bool inside = p.x >= s_clip.x && p.x <= s_clip.z && p.y >= s_clip.y && p.y <= s_clip.w;
        if (inside != (clip_mode == 2u))
            discard;
    }
    uint tsp = s_w.z;
    // the vertex's colors with 8 more bits below the point, as raster() carries them
    float4 vc = clamp(floor(v.v_col * 256.0 + 0.5) / 256.0, 0.0, 255.0);
    float3 c = vc.rgb;
    float ia = (tsp >> 20 & 1u) != 0u ? vc.a : 255.0;
    bool textured = s_tex.y != 0u;
    bool bump = textured && (s_w.w >> 27 & 7u) == 4u;
    bool has_ofs = (s_w.x >> 2 & 1u) != 0u && textured;
    float4 ofs = clamp(floor(v.v_ofs * 256.0 + 0.5) / 256.0, 0.0, 255.0);
    pass_k = s_pad.x;
    pass_ox = int(s_pad.y);
    if (textured) {
        int tw0 = int(s_tex.y), th0 = int(s_tex.z);
        bool clamp_u = (tsp >> 16 & 1u) != 0u, clamp_v = (tsp >> 15 & 1u) != 0u;
        bool flip_u = (tsp >> 18 & 1u) != 0u, flip_v = (tsp >> 17 & 1u) != 0u;
        // the mip-map level: by how many texels a pixel covers, times the polygon's adjustment
        int lv = 0;
        float dadj = float(tsp >> 8 & 15u) * 0.25;
        int maxl = dadj == 0.0 ? 0 : int(s_tex.w) - 1;
        if (maxl > 0) {
            float2 sz = float2(tw0, th0);
            float2 ax = dx * sz, ay = dy * sz;
            float r2 = max(dot(ax, ax), dot(ay, ay)) * dadj * dadj;
            if (r2 > 2.0)
                lv = min(int((asuint(r2) >> 23) - 126u) >> 1, maxl);
        }
        uint at = s_tex.x;
        for (int l = 0; l < lv; l++)
            at += uint((tw0 >> l) * (th0 >> l));
        int tw = tw0 >> lv, th = th0 >> lv;
        float4 t;
        if ((tsp >> 13 & 3u) != 0u && !bump) {
            float fu_ = v.v_uv.x * float(tw) + 16383.5, fv = v.v_uv.y * float(th) + 16383.5;
            int ix = int(fu_), iy = int(fv);
            float wx = floor((fu_ - float(ix)) * 256.0) / 256.0, wy = floor((fv - float(iy)) * 256.0) / 256.0;
            int x0 = tex_at_u(ix - 16384, tw, clamp_u, flip_u), x1 = tex_at_u(ix - 16383, tw, clamp_u, flip_u);
            int y0 = tex_at(iy - 16384, th, clamp_v, flip_v), y1 = tex_at(iy - 16383, th, clamp_v, flip_v);
            float4 top = lerp(texel(at, tw, x0, y0), texel(at, tw, x1, y0), wx);
            float4 bot = lerp(texel(at, tw, x0, y1), texel(at, tw, x1, y1), wx);
            t = lerp(top, bot, wy);
        } else {
            int ix = int(v.v_uv.x * float(tw) + 16384.0) - 16384, iy = int(v.v_uv.y * float(th) + 16384.0) - 16384;
            t = texel(at, tw, tex_at_u(ix, tw, clamp_u, flip_u), tex_at(iy, th, clamp_v, flip_v));
        }
        if (bump) {
            // the texel is two angles, the offset color the light: K1 + K2 sin S + K3 cos S cos (R - Q)
            float s_ang = t.g * (PI / 2.0 / 256.0), r_ang = t.b * (2.0 * PI / 256.0);
            float k1 = ofs.a / 255.0, k2 = ofs.r / 255.0, k3 = ofs.g / 255.0, q = ofs.b * (2.0 * PI / 256.0);
            float bv = clamp((k1 + k2 * sin(s_ang) + k3 * cos(s_ang) * cos(r_ang - q)) * 255.0, 0.0, 255.0);
            t = float4(bv, bv, bv, bv);
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
            c = min(c + ofs.rgb, float3(255.0, 255.0, 255.0));
    }
    if (list == 1u && ia < fu_p0.y)
        discard;
    uint fog_mode = tsp >> 22 & 3u;
    if (fu_p1.x != 0u && (fog_mode == 0u || fog_mode == 3u)) {
        float f = fog_at(v.v_z);
        float3 fc = rgb_of(fu_p1.y);
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
        c = c + (rgb_of(fu_p1.z) - c) * f / 256.0;
    }
    return float4(c, ia) / 255.0;
}
