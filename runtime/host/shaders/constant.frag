#version 450
// One colour: the shadow's scale (multiplied into the target by the
// blend), or nothing at all for the stencil-only passes
layout(location = 0) out vec4 o_col;
layout(set = 3, binding = 0) uniform FU {
    vec4 col;
} fu;

void main()
{
    o_col = fu.col;
}
