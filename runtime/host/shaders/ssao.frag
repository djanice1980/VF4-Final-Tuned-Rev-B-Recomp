#version 450
// Ambient occlusion from the depth buffer alone (docs/design.md section 73):
// how much of the half space over a pixel's surface is taken up by what is
// drawn near it. The depth is zs / Z (larger is nearer, 0 nothing drawn);
// a pixel's place before the eye comes back from it by the scene's
// projection, its surface's normal from its neighbours' places.
layout(location = 0) out vec4 o_col;
layout(set = 2, binding = 0) uniform sampler2D t_depth;
layout(set = 3, binding = 0) uniform FU {
    vec4 proj;          // sx, cx, sy, cy: x = cx + sx X / Z in the game's pixels
    vec4 p;             // render scale, zs, the radius (the scene's units), the strength
} fu;

const int TAPS = 12;

// the place before the eye of what pixel px shows; z 0: nothing there
vec3 place(ivec2 px, ivec2 size)
{
    px = clamp(px, ivec2(0), size - 1);
    float zb = texelFetch(t_depth, px, 0).r;
    if (!(zb > 0.0))
        return vec3(0.0);
    float Z = fu.p.y / zb;
    vec2 g = (vec2(px) + 0.5) / fu.p.x;
    return vec3((g.x - fu.proj.y) / fu.proj.x * Z, (g.y - fu.proj.w) / fu.proj.z * Z, Z);
}

// along one axis: the difference to the neighbour that lies nearer in depth
// (so that an edge's far side does not tilt the normal)
vec3 slope(vec3 c, vec3 a, vec3 b)
{
    vec3 da = c - a, db = b - c;
    if (a.z == 0.0)
        return db;
    if (b.z == 0.0)
        return da;
    return abs(da.z) < abs(db.z) ? da : db;
}

void main()
{
    ivec2 size = textureSize(t_depth, 0);
    ivec2 px = ivec2(gl_FragCoord.xy);
    vec3 P = place(px, size);
    o_col = vec4(1.0);
    if (P.z == 0.0)
        return;
    // the radius on the screen; too small to matter far away
    float rpix = fu.p.z * abs(fu.proj.x) * fu.p.x / P.z;
    if (rpix < 1.5)
        return;
    rpix = min(rpix, 48.0 * fu.p.x);
    vec3 dx = slope(P, place(px - ivec2(1, 0), size), place(px + ivec2(1, 0), size));
    vec3 dy = slope(P, place(px - ivec2(0, 1), size), place(px + ivec2(0, 1), size));
    vec3 N = cross(dx, dy);
    float nl = length(N);
    if (!(nl > 0.0))
        return;
    N /= nl;
    if (dot(N, P) > 0.0)        // towards the eye, which is at the origin
        N = -N;
    // a turn of the taps' spiral for each pixel of a 4 x 4 block: the blur takes the pattern out
    int cell = (px.x & 3) + ((px.y & 3) << 2);
    float turn = float((cell * 7) & 15) * (6.2831853 / 16.0);
    float r2 = fu.p.z * fu.p.z, bias = 0.02 * P.z + 0.004;
    float sum = 0.0;
    for (int i = 0; i < TAPS; i++) {
        float t = (float(i) + 0.5) / float(TAPS);
        float a = turn + float(i) * 2.3999632;
        ivec2 q = px + ivec2(round(vec2(cos(a), sin(a)) * (rpix * sqrt(t))));
        vec3 S = place(q, size);
        if (S.z == 0.0)
            continue;
        vec3 v = S - P;
        float vv = dot(v, v), vn = dot(v, N) - bias;
        // what stands over the surface counts by its angle, and less the farther it is; past the radius, nothing
        float w = clamp(1.0 - vv / r2, 0.0, 1.0);
        sum += max(vn, 0.0) / sqrt(vv + 1e-6) * w;
    }
    float ao = clamp(1.0 - fu.p.w * sum * (2.0 / float(TAPS)), 0.0, 1.0);
    o_col = vec4(ao, ao, ao, 1.0);
}
