// ssao_apply.frag's twin in DirectX's shader language (docs/design.md section 103).
// The smoothed occlusion as a color: the blend multiplies it into the picture, where the stencil
// says the 3D scene is (docs/design.md section 73).
Texture2D<float4> t_ao : register(t0, space2);
SamplerState s_ao : register(s0, space2);

float4 main(float4 pos : SV_Position) : SV_Target0
{
    float ao = t_ao.Load(int3(int2(pos.xy), 0)).r;
    return float4(ao, ao, ao, 1.0);
}
