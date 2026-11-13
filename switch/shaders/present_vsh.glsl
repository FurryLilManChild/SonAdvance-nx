#version 460

// Full-screen quad as a 4-vertex triangle strip, no vertex buffers.
layout (location = 0) out vec2 outUv;

void main()
{
    vec2 pos = vec2(float(gl_VertexID & 1), float((gl_VertexID >> 1) & 1));
    outUv = vec2(pos.x, 1.0 - pos.y);
    gl_Position = vec4(pos * 2.0 - 1.0, 0.0, 1.0);
}
