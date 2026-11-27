// constant.frag's twin in DirectX's shader language (docs/design.md section 103).
// One color: the shadow's scale (multiplied into the target by the blend), or nothing at all for
// the stencil-only passes
cbuffer FU : register(b0, space3) {
    float4 fu_col;
};

float4 main() : SV_Target0
{
    return fu_col;
}
