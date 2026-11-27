// ssao.frag's twin in DirectX's shader language (docs/design.md section 103).
// Ambient occlusion from the depth buffer alone (docs/design.md section 73): how much of the half
// space over a pixel's surface is taken up by what is drawn near it. The depth is zs / Z (larger
// is nearer, 0 nothing drawn); a pixel's place before the eye comes back from it by the scene's
// projection, its surface's normal from its neighbors' places.
Texture2D<float4> t_depth : register(t0, space2);
SamplerState s_depth : register(s0, space2);
cbuffer FU : register(b0, space3) {
    float4 fu_proj;         // sx, cx, sy, cy: x = cx + sx X / Z in the game's pixels
    float4 fu_p;            // render scale, zs, the radius (the scene's units), the strength
};

static const int TAPS = 12;

// the place before the eye of what pixel px shows; z 0: nothing there
float3 place(int2 px, int2 size)
{
    px = clamp(px, int2(0, 0), size - 1);
    float zb = t_depth.Load(int3(px, 0)).r;
    if (!(zb > 0.0))
        return float3(0, 0, 0);
    float Z = fu_p.y / zb;
    float2 g = (float2(px) + 0.5) / fu_p.x;
    return float3((g.x - fu_proj.y) / fu_proj.x * Z, (g.y - fu_proj.w) / fu_proj.z * Z, Z);
}

// along one axis: the difference to the neighbor that lies nearer in depth (so that an edge's far
// side does not tilt the normal)
float3 slope(float3 c, float3 a, float3 b)
{
    float3 da = c - a, db = b - c;
    if (a.z == 0.0)
        return db;
    if (b.z == 0.0)
        return da;
    return abs(da.z) < abs(db.z) ? da : db;
}

float4 main(float4 pos : SV_Position) : SV_Target0
{
    uint sw, sh;
    t_depth.GetDimensions(sw, sh);
    int2 size = int2(sw, sh);
    int2 px = int2(pos.xy);
    float3 P = place(px, size);
    float4 one = float4(1, 1, 1, 1);
    if (P.z == 0.0)
        return one;
    // the radius on the screen; too small to matter far away
    float rpix = fu_p.z * abs(fu_proj.x) * fu_p.x / P.z;
    if (rpix < 1.5)
        return one;
    rpix = min(rpix, 48.0 * fu_p.x);
    float3 dx = slope(P, place(px - int2(1, 0), size), place(px + int2(1, 0), size));
    float3 dy = slope(P, place(px - int2(0, 1), size), place(px + int2(0, 1), size));
    float3 N = cross(dx, dy);
    float nl = length(N);
    if (!(nl > 0.0))
        return one;
    N /= nl;
    if (dot(N, P) > 0.0)        // towards the eye, which is at the origin
        N = -N;
    // a turn of the taps' spiral for each pixel of a 4 x 4 block: the blur takes the pattern out
    int cell = (px.x & 3) + ((px.y & 3) << 2);
    float turn = float((cell * 7) & 15) * (6.2831853 / 16.0);
    float r2 = fu_p.z * fu_p.z, bias = 0.02 * P.z + 0.004;
    float sum = 0.0;
    for (int i = 0; i < TAPS; i++) {
        float t = (float(i) + 0.5) / float(TAPS);
        float a = turn + float(i) * 2.3999632;
        int2 q = px + int2(round(float2(cos(a), sin(a)) * (rpix * sqrt(t))));
        float3 S = place(q, size);
        if (S.z == 0.0)
            continue;
        float3 v = S - P;
        float vv = dot(v, v), vn = dot(v, N) - bias;
        // what stands over the surface counts by its angle, and less the farther it is; past the radius, nothing
        float w = clamp(1.0 - vv / r2, 0.0, 1.0);
        sum += max(vn, 0.0) / sqrt(vv + 1e-6) * w;
    }
    float ao = clamp(1.0 - fu_p.w * sum * (2.0 / float(TAPS)), 0.0, 1.0);
    return float4(ao, ao, ao, 1.0);
}
