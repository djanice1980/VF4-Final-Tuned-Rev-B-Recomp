// rt_apply.frag's twin in DirectX's shader language (docs/design.md section 103).
// The traced light into the picture, where the stencil says the 3D scene is (docs/design.md
// section 74): the picture's color times the light's ratio (the tracer's first output, a quarter
// of the ratio in 10 bits a channel), less the glass's share, plus what the glass mirrors (its
// second output: the mirrored color times the share, and the share).
Texture2D<float4> t_color : register(t0, space2);
Texture2D<float4> t_light : register(t1, space2);
Texture2D<float4> t_refl : register(t2, space2);
SamplerState s_color : register(s0, space2);
SamplerState s_light : register(s1, space2);
SamplerState s_refl : register(s2, space2);

float4 main(float4 pos : SV_Position) : SV_Target0
{
    int3 px = int3(int2(pos.xy), 0);
    float3 c = t_color.Load(px).rgb;
    float3 ratio = t_light.Load(px).rgb * 4.0;
    float4 r = t_refl.Load(px);
    return float4(c * ratio * (1.0 - r.a) + r.rgb, 1.0);
}
