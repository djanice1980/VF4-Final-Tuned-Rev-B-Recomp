#version 450
// A rectangle of the target (two triangles from the vertex number): the
// background, the volumes' stencil resolve, the shadow
layout(set = 1, binding = 0) uniform VU {
    vec4 rect;          // x0, y0, x1, y1 in the target's pixels
    vec4 screen;        // target width, height
} vu;

void main()
{
    const ivec2 corner[6] = ivec2[6](ivec2(0, 0), ivec2(1, 0), ivec2(0, 1), ivec2(1, 0), ivec2(1, 1), ivec2(0, 1));
    ivec2 k = corner[gl_VertexIndex];
    vec2 p = vec2(k.x == 0 ? vu.rect.x : vu.rect.z, k.y == 0 ? vu.rect.y : vu.rect.w);
    gl_Position = vec4(p.x / vu.screen.x * 2.0 - 1.0, 1.0 - p.y / vu.screen.y * 2.0, 0.0, 1.0);
}
