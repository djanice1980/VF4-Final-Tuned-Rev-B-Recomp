// The traced light's ratio and the reflections smoothed, for the DirectX 12 ray tracing
// (docs/design.md sections 74 and 75): rt_filter.comp's twin in DirectX's shader language. Each
// pixel takes the weighted mean of its neighbors' values, the nearer ones counting more, among
// those whose surface is about as far from the eye as its own; the reflections over a smaller
// neighborhood.

cbuffer P : register(b0) {      // rt_light.hlsl's
    float4 p_proj, p_misc, p_shadow_dir;
    int4 p_size;            // picture w, h
    int4 p_base;
    float4 p_dist, p_tune;
    int4 p_opt;
    float4 p_tune2;
    int4 p_counts;          // texels; the ratio's neighborhood, in pixels each way (1 to 3)
};
StructuredBuffer<uint> light_in : register(t0);     // the ratio / 4, 10 bits a channel
StructuredBuffer<float> depth : register(t1);       // how far along the eye's ray; 0: nothing
StructuredBuffer<uint> refl_in : register(t2);      // the mirrored color times its share; the share
RWStructuredBuffer<uint> light_out : register(u0);
RWStructuredBuffer<uint> refl_out : register(u1);

static const int RR = 1;        // the reflection's neighborhood

[numthreads(8, 8, 1)]
void main(uint3 id : SV_DispatchThreadID)
{
    int2 px = int2(id.xy);
    if (px.x >= p_size.x || px.y >= p_size.y)
        return;
    int at = px.y * p_size.x + px.x;
    float zc = depth[at];
    if (!(zc > 0.0)) {
        light_out[at] = light_in[at];
        refl_out[at] = refl_in[at];
        return;
    }
    float3 sum = float3(0, 0, 0);
    float4 rsum = float4(0, 0, 0, 0);
    float n = 0.0, rn = 0.0;
    int R = clamp(p_counts.y, 1, 3);
    for (int y = -R; y <= R; y++)
        for (int x = -R; x <= R; x++) {
            int2 q = clamp(px + int2(x, y), int2(0, 0), p_size.xy - 1);
            int qa = q.y * p_size.x + q.x;
            float z = depth[qa];
            if (!(z > 0.0) || abs(z - zc) > 0.04 * zc + 0.01)
                continue;
            float w = 1.0 / (1.0 + 0.35 * float(x * x + y * y));
            uint l = light_in[qa];
            sum += w * float3(float(l & 1023u), float(l >> 10 & 1023u), float(l >> 20 & 1023u));
            n += w;
            if (abs(x) <= RR && abs(y) <= RR) {
                uint r = refl_in[qa];
                rsum += w * float4(float(r & 255u), float(r >> 8 & 255u), float(r >> 16 & 255u), float(r >> 24));
                rn += w;
            }
        }
    uint3 c = uint3(sum / n + 0.5);
    light_out[at] = c.x | c.y << 10 | c.z << 20 | 0xc0000000u;
    uint4 rc = uint4(rsum / rn + 0.5);
    refl_out[at] = rc.x | rc.y << 8 | rc.z << 16 | rc.w << 24;
}
