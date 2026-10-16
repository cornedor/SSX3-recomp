#version 450

/* One triangle over the viewport; crt.frag works from gl_FragCoord. */
void main()
{
    vec2 q = vec2((gl_VertexIndex << 1) & 2, gl_VertexIndex & 2);
    gl_Position = vec4(q * 2.0 - 1.0, 0.0, 1.0);
}
