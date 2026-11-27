// The traced light of a frame for the DirectX 12 ray tracing (docs/design.md sections 74 and 75):
// rt_light.comp's twin in DirectX's shader language, and to be kept its twin (the two give the
// same picture: section 75's check). A pixel a thread, by the GPU's own ray tracing (inline ray
// queries through the frame's acceleration structures). For the surface the eye's ray meets
// first among the solid lists' triangles, the light's ratio goes out (the game's sum of its
// lights with each light's share cut by what rays towards it meet, and the ambient part gathered
// from what is around, over the game's own sum), what a pane of glass before it mirrors, and
// the eye ray's length for the filter (rt_filter.hlsl). Everything is in the game's view space,
// the eye at the origin.

struct Tri {
    float4 p0, p1, p2, n0, n1, n2;
    float4 uv01;
    float2 uv2;
    uint job;
    uint flags;         // bits 0-1: 0 opaque, 1 by its alpha, 2 punch-through; 4 baked; 8 a fighter's;
                        // 16 one-sided; 32 its front is the other way
    uint4 col;
};
struct Job { float4 base; float4 spec; uint ls, st, flags, pad; };
struct State { uint pcw, isp, tsp, tcw, tex_at, tex_w, tex_h, tex_levels; int4 clip; uint4 pad; };

RaytracingAccelerationStructure tlas : register(t0);
cbuffer P : register(b0) {
    float4 p_proj;          // sx, cx, sy, cy
    float4 p_misc;          // near z, picture pixels a game pixel, alpha reference, the game's shadow scale (of 1)
    float4 p_shadow_dir;    // the way the game's shadow light travels; w 1: known
    int4 p_size;            // picture w, h; ambient rays; shadow rays a light
    int4 p_base;            // the first triangle of each instance
    float4 p_dist;          // ambient rays' reach, shadow rays' reach, the sun's radius, a bulb's radius
    float4 p_tune;          // eps, baked surfaces' share of the occlusion, the most a pixel is brightened, the glass's gain
    int4 p_opt;             // reflection rays, bounce on, point lights' shadows on, seed
    float4 p_tune2;         // the glass's roughness; how much of its angle's share the water's mirror takes
                            // over the game's picture (1: all of it; section 90); 0, 0
    int4 p_counts;          // texels; the filter's neighborhood (rt_filter.hlsl)
    float4 p_mirror;        // the water's plane: its normal towards the eye; w: 0 no water, 1 the mirror in the
                            // place of the game's picture, 2 over that picture (which the renderer has drawn)
    float4 p_mirror_bg;     // what a mirror's ray that leaves the scene shows; w: the plane's place (normal . point)
    float4 p_mirror_fog;    // x: where the fog of the water's picture begins in lf (0: none): 129 steps of the
                            // table, the fog's color, its density, 1 when on
};
StructuredBuffer<Tri> tri : register(t1);
StructuredBuffer<Job> job : register(t2);
StructuredBuffer<State> st : register(t3);
StructuredBuffer<float> lf : register(t4);
StructuredBuffer<uint> tx : register(t5);
RWStructuredBuffer<uint> lightpix : register(u0);    // the ratio / 4: 10 bits each of red, green, blue
RWStructuredBuffer<uint> reflpix : register(u1);     // the mirrored color times its share; the share
RWStructuredBuffer<float> depthpix : register(u2);   // how far the eye's ray went; 0: it met nothing

static const uint ALL = 3, FIGHTERS = 1;
// water (a triangle with flag 64: the game gives it its reflection picture; here the rays mirror
// the scene on it). In the picture's place (p_mirror.w 1): WATER_ANGLE 0: a full mirror at every
// angle, as the game's picture is one; 1: the mirror's share falls towards 2 % where the eye looks
// straight down (as water does), and the rest is WATER_BODY. Over the game's picture (p_mirror.w
// 2): the mirror's share is that angle's share times WATER_OVER, and the game's picture keeps the
// rest. WATER_RIPPLE: how much of each triangle's own tilt the mirror takes (0: the
// flat plane of all of them). WATER_BUMP: how much of the mesh's own height over that plane the
// mirror stands on (the game's water is a grid with heights that it moves on some stages; 1 would
// swing the mirrored picture by twice each height, several times what the game's own does).
static const float WATER_ANGLE = 0.0, WATER_RIPPLE = 0.0, WATER_BUMP = 0.15, WATER_OVER = 1.0;
static const float3 WATER_BODY = float3(0.02, 0.05, 0.07);

// A pixel's rays are not drawn by chance (docs/design.md section 80). The n rays of one use lie
// on a spiral over the disk, one in each ring of equal area. Each pixel turns its spiral by an
// angle of its own and shifts its rings by an offset of its own, and those two are spread evenly
// among the pixels of any small neighborhood (interleaved gradient noise): what the filter then
// averages is rays that cover the disk evenly, where chance leaves clumps and gaps, which is grain.
static float2 pix_shift;        // the pixel's own turn and its rings' offset, each 0..1
static int ray_use0;            // the first use of lights_sum's rays (each light its own)
static int bounces;             // radiance() calls so far in this pixel: each has uses of its own

float ign(float2 q)
{
    return frac(52.9829189 * frac(dot(q, float2(0.06711056, 0.00583715))));
}

// ray i of n of a use: x where in the disk's area (the radius squared, 0..1), y the angle in turns
float2 ray_at(int i, int n, int use)
{
    return float2((float(i) + frac(pix_shift.y + float(use) * 0.7548777)) / float(n),
                  frac(pix_shift.x + float(use) * 0.6180340 + float(i) * 0.3819660));
}

float3 safe_norm(float3 v)
{
    float l = length(v);
    return l > 0.0 ? v / l : v;
}

void basis(float3 n, out float3 a, out float3 b)
{
    float3 h = abs(n.x) < 0.5 ? float3(1, 0, 0) : float3(0, 1, 0);
    a = normalize(cross(n, h));
    b = cross(n, a);
}

float3 cos_dir(float3 n, float2 u)
{
    float u1 = u.x, u2 = u.y * 6.2831853;
    float rr = sqrt(u1), z = sqrt(1.0 - u1);
    float3 a, b;
    basis(n, a, b);
    return normalize(a * (rr * cos(u2)) + b * (rr * sin(u2)) + n * z);
}

float3 disk_jitter(float3 d, float radius, float2 u)
{
    float u1 = sqrt(u.x) * radius, u2 = u.y * 6.2831853;
    float3 a, b;
    basis(d, a, b);
    return normalize(d + a * (u1 * cos(u2)) + b * (u1 * sin(u2)));
}

int wrapc(int t, int size, bool clampit, bool flip)
{
    if (clampit)
        return clamp(t, 0, size - 1);
    if (flip && (t & size) != 0)
        return ~t & (size - 1);
    return t & (size - 1);
}

uint texel(uint s, float2 uv)
{
    int tw = int(st[s].tex_w), th = int(st[s].tex_h);
    if (!(abs(uv.x) < 1e4)) uv.x = 0.0;
    if (!(abs(uv.y) < 1e4)) uv.y = 0.0;
    uint tsp = st[s].tsp;
    int ix = wrapc(int(floor(uv.x * float(tw))), tw, (tsp >> 16 & 1u) != 0u, (tsp >> 18 & 1u) != 0u);
    int iy = wrapc(int(floor(uv.y * float(th))), th, (tsp >> 15 & 1u) != 0u, (tsp >> 17 & 1u) != 0u);
    uint at = st[s].tex_at + uint(iy * tw + ix);
    return at < uint(p_counts.x) ? tx[at] : 0xffffffffu;
}

float3 normal_at(uint ti, float2 b)
{
    return safe_norm(tri[ti].n0.xyz * (1.0 - b.x - b.y) + tri[ti].n1.xyz * b.x + tri[ti].n2.xyz * b.y);
}

float3 face_normal(uint ti)
{
    return safe_norm(cross(tri[ti].p1.xyz - tri[ti].p0.xyz, tri[ti].p2.xyz - tri[ti].p0.xyz));
}

struct Light { float3 col; int flags; float3 dir; float ka; float3 pos; float kb; float ck, cb; int dmode, route; };

Light light_at(uint ls, int k)
{
    uint o = ls * 244u + 4u + uint(k) * 16u;
    Light l;
    l.col = float3(lf[o], lf[o + 1u], lf[o + 2u]);
    l.flags = int(lf[o + 3u]);
    l.dir = float3(lf[o + 4u], lf[o + 5u], lf[o + 6u]);
    l.ka = lf[o + 7u];
    l.pos = float3(lf[o + 8u], lf[o + 9u], lf[o + 10u]);
    l.kb = lf[o + 11u];
    l.ck = lf[o + 12u];
    l.cb = lf[o + 13u];
    l.dmode = int(lf[o + 14u]);
    l.route = int(lf[o + 15u]);
    return l;
}

float3 ambient_of(uint ls)
{
    uint o = ls * 244u;
    return float3(lf[o], lf[o + 1u], lf[o + 2u]);
}

// the direction to a light and its fall with distance; false: no light from it
bool light_dir(Light l, float3 P, out float3 t, out float at, out float dist)
{
    at = 1.0;
    dist = 0.0;
    t = float3(0, 0, 0);
    if ((l.flags & 2) != 0) {
        float3 d = l.pos - P;
        dist = length(d);
        if (dist <= 0.0)
            return false;
        t = d / dist;
        at = clamp((l.flags & 4) != 0 ? l.ka * dist + l.kb : l.ka / dist + l.kb, 0.0, 1.0);
    } else
        t = -l.dir;
    return true;
}

// a spot light's cone at a point that sees the light along t: 1 inside, 0 outside (sections 64, 89)
float light_cone(Light l, float3 t)
{
    if ((l.flags & 2) == 0 || l.ck == 0.0)
        return 1.0;
    float cs = -dot(t, l.dir);
    return clamp(l.ck > 0.0 ? l.ck * cs + l.cb : l.ck * (1.0 - cs) + l.cb, 0.0, 1.0);
}

float alpha_light(uint ls, float3 P, float3 N)
{
    float la = 0.0;
    for (int k = 0; k < 15; k++) {
        Light l = light_at(ls, k);
        if ((l.flags & 1) == 0 || (l.route & 6) != 4)
            continue;
        float3 t;
        float at, dist;
        if (!light_dir(l, P, t, at, dist))
            continue;
        float amt = l.col.x * at;
        if ((l.flags & 2) != 0 && l.ck != 0.0)
            amt *= light_cone(l, t);
        if (l.dmode != 3 && (l.route & 1) == 0)
            amt *= max(dot(N, t), 0.0);
        la += (l.route & 8) != 0 ? -amt : amt;
    }
    return la;
}

float4 unpack(uint c)
{
    return float4(float(c >> 16 & 255u), float(c >> 8 & 255u), float(c & 255u), float(c >> 24));
}

// a point of a triangle: its color before the lights (0..1) and its alpha (0..255)
void surface(uint ti, float2 b, float3 P, out float3 rgb, out float alpha)
{
    uint ji = tri[ti].job;
    uint s = job[ji].st;
    float w = 1.0 - b.x - b.y;
    float3 c;
    float a;
    uint4 col = tri[ti].col;
    if ((col.x | col.y | col.z) != 0u) {
        float4 cc = unpack(col.x) * w + unpack(col.y) * b.x + unpack(col.z) * b.y;
        c = cc.rgb;
        a = cc.a;
    } else {
        c = job[ji].base.rgb;
        a = job[ji].base.a;
        if ((job[ji].flags & 1u) == 0u)
            a = clamp(a + alpha_light(job[ji].ls, P, normal_at(ti, b)) * 255.0, 0.0, 255.0);
    }
    uint tsp = st[s].tsp;
    float ia = (tsp >> 20 & 1u) != 0u ? a : 255.0;
    if (st[s].tex_w != 0u) {
        float2 uv = tri[ti].uv01.xy * w + tri[ti].uv01.zw * b.x + tri[ti].uv2 * b.y;
        uint t = texel(s, uv);
        float3 tc = float3(float(t >> 16 & 255u), float(t >> 8 & 255u), float(t & 255u));
        float ta = (tsp >> 19 & 1u) != 0u ? 255.0 : float(t >> 24);
        uint shade = tsp >> 6 & 3u;
        if ((shade & 1u) != 0u) {
            c = tc * c / 255.0;
            ia = shade == 1u ? ta : ta * ia / 255.0;
        } else if (shade == 0u) {
            c = tc;
            ia = ta;
        } else {
            float k = ta / 255.0;
            c = tc * k + c * (1.0 - k);
        }
    }
    rgb = c / 255.0;
    alpha = ia;
}

float opacity(uint ti, float2 b, float3 P)
{
    uint blend = tri[ti].flags & 3u;
    if (blend == 0u)
        return 1.0;
    float3 c;
    float a;
    surface(ti, b, P, c, a);
    if (blend == 2u)
        return a >= p_misc.z ? 1.0 : 0.0;
    return a / 255.0;
}

// the first triangle of an instance (a constant vector is not indexed by a variable: DXC fails on it for DXIL)
uint base_of(uint g)
{
    return uint(g == 0u ? p_base.x : g == 1u ? p_base.y : g == 2u ? p_base.z : p_base.w);
}

struct Hit { bool ok; float t; float2 b; uint tri; };

RayDesc ray_of(float3 o, float3 d, float tmin, float tmax)
{
    RayDesc r;
    r.Origin = o;
    r.Direction = d;
    r.TMin = tmin;
    r.TMax = tmax;
    return r;
}

// the nearest thing a ray meets that is at least half opaque. mode 1: the eye's ray (the solid lists
// only, and a one-sided triangle seen from behind is not there); 2: only glass (a translucent pane of the stage with a
// highlight of its own, mostly clear)
Hit closest(float3 o, float3 d, float tmin, float tmax, int mode, uint mask)
{
    RayQuery<RAY_FLAG_NONE> q;
    q.TraceRayInline(tlas, mode != 0 ? RAY_FLAG_FORCE_NON_OPAQUE : RAY_FLAG_NONE, mask, ray_of(o, d, tmin, tmax));
    while (q.Proceed()) {
        if (q.CandidateType() != CANDIDATE_NON_OPAQUE_TRIANGLE)
            continue;
        uint ti = base_of(q.CandidateInstanceID()) + q.CandidatePrimitiveIndex();
        float2 b = q.CandidateTriangleBarycentrics();
        float t = q.CandidateTriangleRayT();
        uint fl = tri[ti].flags;
        if (mode == 1 && (fl & 3u) == 1u)
            continue;       // the translucent list is drawn over the picture afterwards
        if (mode == 3 && (fl & 64u) != 0u)
            continue;       // a mirror's own ray: the water is not in its way
        if (mode != 0 && (fl & 16u) != 0u) {
            float front = (fl & 32u) != 0u ? -1.0 : 1.0;
            if (dot(face_normal(ti), d) * front >= 0.0)
                continue;
        }
        if (mode == 2) {
            uint ji = tri[ti].job;
            if ((fl & 3u) != 1u || (fl & 12u) != 0u || (job[ji].flags & 0x80u) == 0u || !(job[ji].spec.w > 0.0) ||
                !(job[ji].spec.x + job[ji].spec.y + job[ji].spec.z > 0.0) || opacity(ti, b, o + d * t) >= 0.5)
                continue;
        } else if ((fl & 3u) != 0u && opacity(ti, b, o + d * t) < 0.5)
            continue;
        q.CommitNonOpaqueTriangleHit();
    }
    Hit h;
    h.ok = q.CommittedStatus() == COMMITTED_TRIANGLE_HIT;
    h.t = tmax;
    h.b = float2(0, 0);
    h.tri = 0u;
    if (h.ok) {
        h.t = q.CommittedRayT();
        h.b = q.CommittedTriangleBarycentrics();
        h.tri = base_of(q.CommittedInstanceID()) + q.CommittedPrimitiveIndex();
    }
    return h;
}

// how much light gets along a ray past the fighters: 1 nothing in the way, 0 something opaque
float transmit(float3 o, float3 d, float tmin, float tmax)
{
    RayQuery<RAY_FLAG_NONE> q;
    q.TraceRayInline(tlas, RAY_FLAG_ACCEPT_FIRST_HIT_AND_END_SEARCH, FIGHTERS, ray_of(o, d, tmin, tmax));
    float T = 1.0;
    while (q.Proceed()) {
        if (q.CandidateType() != CANDIDATE_NON_OPAQUE_TRIANGLE)
            continue;
        uint ti = base_of(q.CandidateInstanceID()) + q.CandidatePrimitiveIndex();
        T *= 1.0 - opacity(ti, q.CandidateTriangleBarycentrics(), o + d * q.CandidateTriangleRayT());
        if (T < 0.01)
            q.CommitNonOpaqueTriangleHit();
    }
    if (q.CommittedStatus() == COMMITTED_TRIANGLE_HIT)
        return 0.0;
    return T;
}

float game_shadow(float3 O)
{
    int n = max(p_size.w, 1);
    float vis = 0.0;
    for (int i = 0; i < n; i++)
        vis += transmit(O, disk_jitter(-p_shadow_dir.xyz, p_dist.z, ray_at(i, n, 1)), 1e-4, p_dist.y);
    // a soft edge (some of the rays pass, some do not): twice as many again, where they tell something
    if (n > 1 && vis > 0.0 && vis < float(n)) {
        int m = 2 * n;
        for (int j = 0; j < m; j++)
            vis += transmit(O, disk_jitter(-p_shadow_dir.xyz, p_dist.z, ray_at(j, m, 2)), 1e-4, p_dist.y);
        n += m;
    }
    vis /= float(n);
    return p_misc.w + (1.0 - p_misc.w) * vis;
}

// the game's sum for a point with normal N. n > 0: each light's share times what n rays from O find
float3 lights_sum(uint ls, float3 P, float3 N, float3 O, int n, float3 amb)
{
    float3 sum = amb;
    for (int k = 0; k < 15; k++) {
        Light l = light_at(ls, k);
        if ((l.flags & 1) == 0 || (l.route & 6) == 4)
            continue;
        float3 t;
        float at, dist;
        if (!light_dir(l, P, t, at, dist))
            continue;
        at *= light_cone(l, t);     // a spot that goes to the colour lights inside its cone only (section 89)
        float d = dot(N, t) * at;
        if (d <= 0.0)
            continue;
        float vis = 1.0;
        bool point_light = (l.flags & 2) != 0;
        if (n > 0 && (!point_light || p_opt.z != 0)) {
            vis = 0.0;
            // n rays; where some pass and some do not (a shadow's soft edge), twice as many again
            int m = n, first = 0, use = ray_use0 + k;
            for (int turn = 0; turn < 2; turn++) {
                for (int i = 0; i < m; i++) {
                    float2 u = ray_at(i, m, use);
                    if (point_light) {
                        float3 target = l.pos + (float3(u, frac(u.x * 7.0 + u.y * 13.0 + pix_shift.y)) - 0.5) * (2.0 * p_dist.w);
                        float3 dd = target - O;
                        float dl = length(dd);
                        vis += transmit(O, dd / dl, 1e-4, min(dl, p_dist.y));
                    } else
                        vis += transmit(O, disk_jitter(t, p_dist.z, u), 1e-4, p_dist.y);
                }
                first += m;
                if (turn == 1 || n < 2 || vis <= 0.0 || vis >= float(n))
                    break;
                m = 2 * n;
                use += 100;
            }
            vis /= float(first);
        }
        sum += l.col * (d * vis);
    }
    return sum;
}

// where rays from a point of a triangle start: on the smooth surface its vertex normals describe, then off it
float3 ray_origin(uint ti, float2 b, float3 P, float3 nf)
{
    float w = 1.0 - b.x - b.y;
    float3 n0 = tri[ti].n0.xyz, n1 = tri[ti].n1.xyz, n2 = tri[ti].n2.xyz;
    if (dot(n0, nf) > 0.0 && dot(n1, nf) > 0.0 && dot(n2, nf) > 0.0) {
        float d0 = min(dot(P - tri[ti].p0.xyz, n0), 0.0), d1 = min(dot(P - tri[ti].p1.xyz, n1), 0.0),
              d2 = min(dot(P - tri[ti].p2.xyz, n2), 0.0);
        P -= n0 * (w * d0) + n1 * (b.x * d1) + n2 * (b.y * d2);
    }
    return P + nf * p_tune.x;
}

// the game's fog on a color the water mirrors (0..1), as the game's own picture of the mirrored
// scene has it: by the table, for a point of triangle ti that lies `away` before the mirrored eye
// (scene.frag's fog_at, and its sum for a polygon that takes the table's fog)
float3 water_fog(float3 c, uint ti, float away)
{
    uint at = uint(p_mirror_fog.x);
    if (at == 0u || lf[at + 133u] == 0.0 || (st[job[tri[ti].job].st].tsp >> 22 & 3u) != 0u || !(away > 0.0))
        return c;
    float s = clamp(lf[at + 132u] / away, 1.0, 255.9999);
    uint b = asuint(s);
    uint i = (b >> 19) - (127u << 4), fr = b >> 11 & 255u;
    float f = floor((lf[at + i] * float(256u - fr) + lf[at + i + 1u] * float(fr)) / 256.0);
    f += floor(f / 128.0);
    float3 fc = float3(lf[at + 129u], lf[at + 130u], lf[at + 131u]) / 255.0;
    return c + (fc - c) * (f / 256.0);
}

// what the water at P shows to a ray d that is not the eye's (bounced light, a pane of glass): the
// scene mirrored once in the water's plane, by its colors and the game's lights without shadows
float3 water_seen(float3 P, float3 d)
{
    float3 Nw = dot(p_mirror.xyz, d) < 0.0 ? p_mirror.xyz : -p_mirror.xyz;
    float3 R = normalize(d - Nw * (2.0 * dot(d, Nw)));
    float3 O = P + Nw * p_tune.x;
    Hit q = closest(O, R, 1e-4, 1e9, 3, ALL);
    if (!q.ok)
        return p_mirror_bg.rgb;
    float3 Q = O + R * q.t, c;
    float a;
    surface(q.tri, q.b, Q, c, a);
    if ((tri[q.tri].flags & 4u) != 0u)
        return c;
    uint ls = job[tri[q.tri].job].ls;
    // the product held to full, not the sum (docs/design.md section 98), as the picture's own light
    return min(c * lights_sum(ls, Q, normal_at(q.tri, q.b), Q, 0, ambient_of(ls)), float3(1, 1, 1));
}

// what a surface point sends back along a ray that met it: its color times the game's sum for it
float3 radiance(Hit h, float3 o, float3 d)
{
    float3 P = o + d * h.t, c;
    float a;
    if ((tri[h.tri].flags & 64u) != 0u)
        return water_seen(P, d);    // its own picture is not drawn
    surface(h.tri, h.b, P, c, a);
    if ((tri[h.tri].flags & 4u) != 0u)
        return c;
    float3 N = normal_at(h.tri, h.b), ng = face_normal(h.tri);
    float3 nf = dot(ng, d) < 0.0 ? ng : -ng;
    float3 O = ray_origin(h.tri, h.b, P, nf);
    uint ls = job[tri[h.tri].job].ls;
    int was = ray_use0;
    ray_use0 = 32 + 16 * bounces++;
    float3 sum = lights_sum(ls, P, N, O, 1, ambient_of(ls));
    ray_use0 = was;
    return min(c * sum, float3(1, 1, 1));       // the product held to full, not the sum (section 98)
}

[numthreads(8, 8, 1)]
void main(uint3 id : SV_DispatchThreadID)
{
    int2 px = int2(id.xy);
    if (px.x >= p_size.x || px.y >= p_size.y)
        return;
    uint at = uint(px.y * p_size.x + px.x);
    // p_opt.w: the frame's number among those whose rays differ (1: the same rays every frame)
    float2 pq = float2(px) + 5.588238 * float(p_opt.w - 1);
    pix_shift = float2(ign(pq), ign(pq.yx + float2(47.0, 17.0)));
    ray_use0 = 16;
    bounces = 0;
    float2 g = (float2(px) + 0.5) / p_misc.y;
    float3 d = normalize(float3(-(g.x - p_proj.y) / p_proj.x, (g.y - p_proj.w) / p_proj.z, -1.0));
    float3 o0 = float3(0, 0, 0);
    float tnear = p_misc.x > 0.0 ? p_misc.x / -d.z : 0.05;
    float3 ratio = float3(1, 1, 1), T = float3(0, 0, 0);     // T: the traced sum, for the ratio
    Hit h = closest(o0, d, tnear, 1e9, 1, ALL);
    if (h.ok) {
        uint ti = h.tri, ji = tri[ti].job, s = job[ji].st, ls = job[ji].ls;
        uint fl = tri[ti].flags;
        float3 P = o0 + d * h.t, N = normal_at(ti, h.b), ng = face_normal(ti);
        float3 nf = dot(ng, d) < 0.0 ? ng : -ng;
        float3 O = ray_origin(ti, h.b, P, nf);
        float3 Nh = dot(N, nf) > 0.0 ? N : nf;
        float3 la = ambient_of(ls), amb = float3(0, 0, 0);
        float open = 0.0;
        bool baked = (fl & 4u) != 0u;
        bool takes = (st[s].pcw >> 7 & 1u) != 0u && p_shadow_dir.w != 0.0;
        // A surface with painted light takes nothing from the rays around it but how open it is to
        // the fighters, and that only where the game's shadow falls on it (below): three times the
        // rays there, none of them shaded, and none at all on the others.
        int nao = baked ? (takes ? 3 * p_size.z : 0) : p_size.z;
        for (int i = 0; i < nao; i++) {
            float3 rd = cos_dir(Nh, ray_at(i, nao, 0));
            Hit q = closest(O, rd, 1e-4, p_dist.x, 0, ALL);
            if (q.ok) {
                float f = q.t / p_dist.x;
                float w = f < 0.5 ? 1.0 : 1.0 - (f - 0.5) * 2.0;
                w = w * w * (3.0 - 2.0 * w);
                if (!baked) {
                    float3 L = float3(0, 0, 0);
                    if (p_opt.y != 0)
                        L = radiance(q, O, rd);
                    amb += w * L + (1.0 - w) * la;
                }
                open += (tri[q.tri].flags & 8u) != 0u || !baked ? 1.0 - w : 1.0;
            } else {
                amb += la;
                open += 1.0;
            }
        }
        if (nao > 0) {
            amb /= float(nao);
            open /= float(nao);
        } else {
            amb = la;
            open = 1.0;
        }
        if (!baked) {
            float gs = 1.0;
            if (takes)
                gs = game_shadow(O);
            float3 G = lights_sum(ls, P, N, O, 0, la);
            T = lights_sum(ls, P, N, O, takes ? 0 : p_size.w, amb) * gs;
            ratio = T / max(G, float3(1.0 / 255.0, 1.0 / 255.0, 1.0 / 255.0));
            float top = (fl & 3u) != 0u ? 1.0 : p_tune.z;
            ratio = min(ratio, float3(top, top, top));
            T = min(G, float3(1, 1, 1)) * ratio;
        } else {
            float f = 1.0;
            if (takes)
                f = game_shadow(O) * (1.0 - p_tune.y * (1.0 - open));
            ratio = float3(f, f, f);
            T = float3(f, f, f);
        }
    }
    // water: the scene mirrored in its plane, in the place of the game's reflection picture, by
    // the game's own colors for the polygon (mode 3: like the eye's ray, without the water)
    float3 wr = float3(0, 0, 0);
    float wF = 0.0;
    if (h.ok && p_mirror.w != 0.0 && (tri[h.tri].flags & 64u) != 0u) {
        float3 Pw = o0 + d * h.t, ngw = face_normal(h.tri);
        float3 Nw = dot(p_mirror.xyz, d) < 0.0 ? p_mirror.xyz : -p_mirror.xyz;
        float3 Nm = normalize(lerp(Nw, dot(ngw, d) < 0.0 ? ngw : -ngw, WATER_RIPPLE));
        float3 Rw = normalize(d - Nm * (2.0 * dot(d, Nm)));
        if (dot(Rw, Nw) < 0.02)
            Rw = normalize(Rw + Nw * (0.02 - dot(Rw, Nw)));
        float over = dot(p_mirror.xyz, Pw) - p_mirror_bg.w, along = dot(p_mirror.xyz, d);
        if (abs(over) < 0.5 && abs(along) > 1e-4)
            Pw -= d * (over / along * (1.0 - WATER_BUMP));
        float3 Ow = Pw + Nw * p_tune.x;
        Hit q = closest(Ow, Rw, 1e-4, 1e9, 3, ALL);
        // (how far the mirrored point lies before the mirrored eye: the two rays' lengths along the eye's)
        float3 seen = p_mirror_bg.rgb;
        if (q.ok)
            seen = water_fog(radiance(q, Ow, Rw), q.tri, (length(Pw) + q.t) * -d.z);
        uint4 wc = tri[h.tri].col;
        float3 tint = job[tri[h.tri].job].base.rgb / 255.0, add = float3(0, 0, 0);
        if ((wc.x | wc.y | wc.z) != 0u) {
            float w0 = 1.0 - h.b.x - h.b.y;
            tint = (unpack(wc.x).rgb * w0 + unpack(wc.y).rgb * h.b.x + unpack(wc.z).rgb * h.b.y) / 255.0;
            add = unpack(wc.w).rgb / 255.0;
        }
        float angle = 0.02 + 0.98 * pow(1.0 - max(-dot(d, Nm), 0.0), 5.0);
        // the game's sum for the polygon (its picture times its color, plus what it adds), and
        // the light's ratio on it: the shadow the game lets fall on some stages' water
        if (p_mirror.w > 1.5) {
            wr = (seen * tint + add) * min(ratio, float3(1, 1, 1));
            wF = clamp(angle * WATER_OVER * p_tune2.y, 0.0, 1.0);
        } else {
            float share = lerp(1.0, angle, WATER_ANGLE);
            wr = ((seen * share + WATER_BODY * (1.0 - share)) * tint + add) * min(ratio, float3(1, 1, 1));
            wF = 1.0;
        }
    }
    // glass in front of that: what a mirror there would show, by the glass's share at this angle
    float3 rf = float3(0, 0, 0);
    float F = 0.0;
    if (p_opt.x > 0) {
        Hit gl = closest(o0, d, tnear, h.ok ? h.t : 1e9, 2, ALL);
        if (gl.ok) {
            float3 Pg = o0 + d * gl.t, ng = face_normal(gl.tri);
            float3 nfg = dot(ng, d) < 0.0 ? ng : -ng;
            float3 Ng = normal_at(gl.tri, gl.b);
            if (dot(Ng, nfg) <= 0.9)
                Ng = nfg;
            if (nfg.y >= 0.9) {         // this test's hand-marking: the panes that lie in the floor
                float cs = max(-dot(d, Ng), 0.0);
                F = min((0.04 + 0.96 * pow(1.0 - cs, 5.0)) * p_tune.w, 1.0);
                float3 Og = Pg + nfg * p_tune.x;
                for (int i = 0; i < p_opt.x; i++) {
                    float3 nn = disk_jitter(Ng, p_opt.x > 1 ? p_tune2.x : 0.0, ray_at(i, p_opt.x, 3));
                    float3 R = normalize(d - nn * (2.0 * dot(d, nn)));
                    if (dot(R, nfg) > 0.0) {
                        Hit q = closest(Og, R, 1e-4, 1e9, 0, ALL);
                        if (q.ok)
                            rf += radiance(q, Og, R) / float(p_opt.x);
                    }
                }
            }
        }
    }
    depthpix[at] = h.ok ? h.t : 0.0;
    uint3 l = uint3(clamp(ratio * 0.25, 0.0, 1.0) * 1023.0 + 0.5);
    lightpix[at] = l.x | l.y << 10 | l.z << 20 | 0xc0000000u;
    // the water behind a pane of glass keeps what the glass leaves of it
    float3 rc = rf * F + wr * (wF * (1.0 - F));
    float rs = F + wF * (1.0 - F);
    uint3 c = uint3(clamp(rc, 0.0, 1.0) * 255.0 + 0.5);
    reflpix[at] = c.x | c.y << 8 | c.z << 16 | uint(clamp(rs, 0.0, 1.0) * 255.0 + 0.5) << 24;
}
