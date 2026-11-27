// volume.vert's twin in DirectX's shader language (docs/design.md section 103).
// A modifier volume's triangle: the target's pixels and z = 1/w; only its depth matters (it
// toggles the stencil where it lies in front)
cbuffer VU : register(b0, space1) {
    float4 vu_screen;       // target width, height, depth scale, 0
};

// (precise: the position's sum as written, step by step, as the twin's and the scene's are)
void main(float3 a_pos : TEXCOORD0, out precise float4 pos : SV_Position)
{
    precise float2 ndc = float2(a_pos.x / vu_screen.x * 2.0 - 1.0, 1.0 - a_pos.y / vu_screen.y * 2.0);
    pos = float4(ndc, clamp(a_pos.z * vu_screen.z, 0.0, 1.0), 1.0);
}
