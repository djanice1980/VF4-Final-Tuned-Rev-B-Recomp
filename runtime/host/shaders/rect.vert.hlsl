// rect.vert's twin in DirectX's shader language (docs/design.md section 103).
// A rectangle of the target (two triangles from the vertex number): the background, the volumes'
// stencil resolve, the shadow
cbuffer VU : register(b0, space1) {
    float4 vu_rect;         // x0, y0, x1, y1 in the target's pixels
    float4 vu_screen;       // target width, height
};

void main(uint vid : SV_VertexID, out precise float4 pos : SV_Position)
{
    static const int2 corner[6] = { int2(0, 0), int2(1, 0), int2(0, 1), int2(1, 0), int2(1, 1), int2(0, 1) };
    int2 k = corner[vid];
    float2 p = float2(k.x == 0 ? vu_rect.x : vu_rect.z, k.y == 0 ? vu_rect.y : vu_rect.w);
    pos = float4(p.x / vu_screen.x * 2.0 - 1.0, 1.0 - p.y / vu_screen.y * 2.0, 0.0, 1.0);
}
