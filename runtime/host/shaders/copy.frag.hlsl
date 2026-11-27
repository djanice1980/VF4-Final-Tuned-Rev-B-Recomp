// copy.frag's twin in DirectX's shader language (docs/design.md section 103).
// A picture into another target as it is: ray tracing's second color target starts as the first's
// copy (docs/design.md section 74).
Texture2D<float4> t_color : register(t0, space2);
SamplerState s_color : register(s0, space2);

float4 main(float4 pos : SV_Position) : SV_Target0
{
    return t_color.Load(int3(int2(pos.xy), 0));
}
