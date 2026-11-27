// ssao_blur.frag's twin in DirectX's shader language (docs/design.md section 103).
// The occlusion smoothed over the 4 x 4 block its pattern repeats in, among the pixels at about
// the same depth only: an edge keeps its side (docs/design.md section 73).
Texture2D<float4> t_ao : register(t0, space2);
Texture2D<float4> t_depth : register(t1, space2);
SamplerState s_ao : register(s0, space2);
SamplerState s_depth : register(s1, space2);

float4 main(float4 pos : SV_Position) : SV_Target0
{
    uint sw, sh;
    t_ao.GetDimensions(sw, sh);
    int2 size = int2(sw, sh);
    int2 px = int2(pos.xy);
    float zc = t_depth.Load(int3(px, 0)).r;
    float sum = 0.0, n = 0.0;
    for (int y = -2; y < 2; y++)
        for (int x = -2; x < 2; x++) {
            int2 q = clamp(px + int2(x, y), int2(0, 0), size - 1);
            float z = t_depth.Load(int3(q, 0)).r;
            // depth is 1 / distance: within a tenth of the distance
            if (abs(z - zc) <= 0.1 * zc) {
                sum += t_ao.Load(int3(q, 0)).r;
                n += 1.0;
            }
        }
    float ao = n > 0.0 ? sum / n : t_ao.Load(int3(px, 0)).r;
    return float4(ao, ao, ao, 1.0);
}
