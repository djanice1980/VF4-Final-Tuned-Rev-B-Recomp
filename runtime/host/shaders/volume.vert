#version 450
// A modifier volume's triangle: the target's pixels and z = 1/w; only its
// depth matters (it toggles the stencil where it lies in front)
layout(location = 0) in vec3 a_pos;
layout(set = 1, binding = 0) uniform VU {
    vec4 screen;        // target width, height, depth scale, 0
} vu;

void main()
{
    vec2 ndc = vec2(a_pos.x / vu.screen.x * 2.0 - 1.0, 1.0 - a_pos.y / vu.screen.y * 2.0);
    gl_Position = vec4(ndc, clamp(a_pos.z * vu.screen.z, 0.0, 1.0), 1.0);
}
