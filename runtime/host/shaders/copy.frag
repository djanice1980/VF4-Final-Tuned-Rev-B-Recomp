#version 450
// A picture into another target as it is: ray tracing's second colour target
// starts as the first's copy (docs/design.md section 74).
layout(location = 0) out vec4 o_col;
layout(set = 2, binding = 0) uniform sampler2D t_color;

void main()
{
    o_col = texelFetch(t_color, ivec2(gl_FragCoord.xy), 0);
}
