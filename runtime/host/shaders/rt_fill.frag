#version 450
// The traced light into the picture when fewer pixels were traced than the picture has (Ray
// tracing quality, docs/design.md section 76). rt_apply.frag's sum, with the traced pictures read
// between their pixels: each picture pixel takes the traced pixels around it by how near they
// lie, but only those whose eye ray went about as far as this pixel's surface is, so that a
// fighter's light does not run over its outline onto the floor behind. The picture's own depth
// buffer tells how far that is (zs / Z, as ssao.frag reads it). A pixel where nothing is drawn
// keeps its color, and the pass after this one puts the flat parts of the picture back.
layout(location = 0) out vec4 o_col;
layout(set = 2, binding = 0) uniform sampler2D t_color;
layout(set = 2, binding = 1) uniform sampler2D t_light;     // traced: a quarter of the light's ratio
layout(set = 2, binding = 2) uniform sampler2D t_refl;      // traced: the mirrored color times the glass's share; the share
layout(set = 2, binding = 3) uniform sampler2D t_reach;     // traced: how far the eye's ray went; 0: it met nothing
layout(set = 2, binding = 4) uniform sampler2D t_depth;
layout(set = 3, binding = 0) uniform FU {
    vec4 proj;          // sx, cx, sy, cy: x = cx + sx X / Z in the game's pixels
    vec4 p;             // render scale, zs, traced pixels a picture pixel across and down
} fu;

void main()
{
    ivec2 px = ivec2(gl_FragCoord.xy);
    vec3 c = texelFetch(t_color, px, 0).rgb;
    o_col = vec4(c, 1.0);
    float zb = texelFetch(t_depth, px, 0).r;
    if (!(zb > 0.0))
        return;
    // how far along its eye ray this pixel's surface is, as the tracer measures it
    vec2 g = (vec2(px) + 0.5) / fu.p.x;
    vec2 e = vec2((g.x - fu.proj.y) / fu.proj.x, (g.y - fu.proj.w) / fu.proj.z);
    float far = fu.p.y / zb * sqrt(1.0 + dot(e, e));
    float tol = 0.04 * far + 0.01;      // rt_filter.comp's measure of "about as far"
    ivec2 size = textureSize(t_light, 0);
    vec2 u = (vec2(px) + 0.5) * fu.p.zw - 0.5;
    ivec2 i0 = ivec2(floor(u));
    vec2 fr = u - vec2(i0);
    vec3 lsum = vec3(0.0);
    vec4 rsum = vec4(0.0);
    float wsum = 0.0;
    for (int y = 0; y < 2; y++)
        for (int x = 0; x < 2; x++) {
            ivec2 q = clamp(i0 + ivec2(x, y), ivec2(0), size - 1);
            float t = texelFetch(t_reach, q, 0).r;
            if (!(t > 0.0) || abs(t - far) > tol)
                continue;
            float w = (x == 0 ? 1.0 - fr.x : fr.x) * (y == 0 ? 1.0 - fr.y : fr.y) + 1e-3;
            lsum += w * texelFetch(t_light, q, 0).rgb;
            rsum += w * texelFetch(t_refl, q, 0);
            wsum += w;
        }
    if (wsum == 0.0) {
        // none of the four is of this surface (something thin, or an outline the tracer drew a
        // pixel aside): of the sixteen around, the one whose ray went nearest to as far
        float best = 1e30;
        ivec2 bq = ivec2(-1);
        for (int y = -1; y < 3; y++)
            for (int x = -1; x < 3; x++) {
                ivec2 q = clamp(i0 + ivec2(x, y), ivec2(0), size - 1);
                float t = texelFetch(t_reach, q, 0).r;
                if (t > 0.0 && abs(t - far) < best) {
                    best = abs(t - far);
                    bq = q;
                }
            }
        if (bq.x < 0)
            return;         // nothing traced anywhere near: the game's own light
        lsum = texelFetch(t_light, bq, 0).rgb;
        rsum = texelFetch(t_refl, bq, 0);
        wsum = 1.0;
    }
    vec3 ratio = lsum / wsum * 4.0;
    vec4 r = rsum / wsum;
    o_col = vec4(c * ratio * (1.0 - r.a) + r.rgb, 1.0);
}
