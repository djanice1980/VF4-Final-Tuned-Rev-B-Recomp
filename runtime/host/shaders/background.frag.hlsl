// background.frag's twin in DirectX's shader language (docs/design.md section 103).
// The background plane (view.c, target_clear): a color over the screen from three corners, with
// the table's fog of its depth
cbuffer FU : register(b0, space3) {
    float4 fu_c0;           // color at (x0, y0), 0..255; w: render scale
    float4 fu_ga;           // its change along x; w: x0
    float4 fu_gb;           // along y; w: y0
    float4 fu_fog;          // fog color 0..255, fog amount 0..256
};

float4 main(float4 pos : SV_Position) : SV_Target0
{
    float2 p = pos.xy / fu_c0.w;
    // the color and its fog keep their fractions (docs/design.md section 95)
    float3 v = clamp(fu_c0.rgb + fu_ga.rgb * (p.x - fu_ga.w) + fu_gb.rgb * (p.y - fu_gb.w), 0.0, 255.0);
    v = v + (fu_fog.rgb - v) * fu_fog.w / 256.0;
    return float4(v / 255.0, 1.0);
}
