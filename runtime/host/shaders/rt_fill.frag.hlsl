// rt_fill.frag's twin in DirectX's shader language (docs/design.md section 103).
// The traced light into the picture when fewer pixels were traced than the picture has (Ray
// tracing quality, docs/design.md section 76). rt_apply.frag's sum, with the traced pictures read
// between their pixels: each picture pixel takes the traced pixels around it by how near they
// lie, but only those whose eye ray went about as far as this pixel's surface is, so that a
// fighter's light does not run over its outline onto the floor behind. The picture's own depth
// buffer tells how far that is (zs / Z, as ssao.frag reads it). A pixel where nothing is drawn
// keeps its color, and the pass after this one puts the flat parts of the picture back.
Texture2D<float4> t_color : register(t0, space2);
Texture2D<float4> t_light : register(t1, space2);       // traced: a quarter of the light's ratio
Texture2D<float4> t_refl : register(t2, space2);        // traced: the mirrored color times the glass's share; the share
Texture2D<float4> t_reach : register(t3, space2);       // traced: how far the eye's ray went; 0: it met nothing
Texture2D<float4> t_depth : register(t4, space2);
SamplerState s_color : register(s0, space2);
SamplerState s_light : register(s1, space2);
SamplerState s_refl : register(s2, space2);
SamplerState s_reach : register(s3, space2);
SamplerState s_depth : register(s4, space2);
cbuffer FU : register(b0, space3) {
    float4 fu_proj;         // sx, cx, sy, cy: x = cx + sx X / Z in the game's pixels
    float4 fu_p;            // render scale, zs, traced pixels a picture pixel across and down
};

float4 main(float4 pos : SV_Position) : SV_Target0
{
    int2 px = int2(pos.xy);
    float3 c = t_color.Load(int3(px, 0)).rgb;
    float4 own = float4(c, 1.0);
    float zb = t_depth.Load(int3(px, 0)).r;
    if (!(zb > 0.0))
        return own;
    // how far along its eye ray this pixel's surface is, as the tracer measures it
    float2 g = (float2(px) + 0.5) / fu_p.x;
    float2 e = float2((g.x - fu_proj.y) / fu_proj.x, (g.y - fu_proj.w) / fu_proj.z);
    float away = fu_p.y / zb * sqrt(1.0 + dot(e, e));
    float tol = 0.04 * away + 0.01;     // the filter's measure of "about as far"
    uint sw, sh;
    t_light.GetDimensions(sw, sh);
    int2 size = int2(sw, sh);
    float2 u = (float2(px) + 0.5) * fu_p.zw - 0.5;
    int2 i0 = int2(floor(u));
    float2 fr = u - float2(i0);
    float3 lsum = float3(0, 0, 0);
    float4 rsum = float4(0, 0, 0, 0);
    float wsum = 0.0;
    for (int y = 0; y < 2; y++)
        for (int x = 0; x < 2; x++) {
            int2 q = clamp(i0 + int2(x, y), int2(0, 0), size - 1);
            float t = t_reach.Load(int3(q, 0)).r;
            if (!(t > 0.0) || abs(t - away) > tol)
                continue;
            float w = (x == 0 ? 1.0 - fr.x : fr.x) * (y == 0 ? 1.0 - fr.y : fr.y) + 1e-3;
            lsum += w * t_light.Load(int3(q, 0)).rgb;
            rsum += w * t_refl.Load(int3(q, 0));
            wsum += w;
        }
    if (wsum == 0.0) {
        // none of the four is of this surface (something thin, or an outline the tracer drew a
        // pixel aside): of the sixteen around, the one whose ray went nearest to as far
        float best = 1e30;
        int2 bq = int2(-1, -1);
        for (int y2 = -1; y2 < 3; y2++)
            for (int x2 = -1; x2 < 3; x2++) {
                int2 q = clamp(i0 + int2(x2, y2), int2(0, 0), size - 1);
                float t = t_reach.Load(int3(q, 0)).r;
                if (t > 0.0 && abs(t - away) < best) {
                    best = abs(t - away);
                    bq = q;
                }
            }
        if (bq.x < 0)
            return own;     // nothing traced anywhere near: the game's own light
        lsum = t_light.Load(int3(bq, 0)).rgb;
        rsum = t_refl.Load(int3(bq, 0));
        wsum = 1.0;
    }
    float3 ratio = lsum / wsum * 4.0;
    float4 r = rsum / wsum;
    return float4(c * ratio * (1.0 - r.a) + r.rgb, 1.0);
}
