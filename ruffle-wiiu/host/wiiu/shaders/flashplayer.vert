#version 450
// Same math as the PC shader in gfx_gl.c. Loose uniforms (register mode).
layout(location = 0) in vec2 pos;
layout(location = 1) in vec4 color;

uniform vec4 view;   // pixels -> clip: x * view.x + view.z, y * view.y + view.w
uniform vec4 world0; // a, b, c, d
uniform vec4 world1; // tx, ty
uniform vec4 uv0;    // uv matrix a, b, c, d
uniform vec4 uv1;    // uv matrix tx, ty

layout(location = 0) out vec4 v_color;
layout(location = 1) out vec2 v_uv;

void main() {
    vec2 p = vec2(world0.x * pos.x + world0.z * pos.y + world1.x,
                  world0.y * pos.x + world0.w * pos.y + world1.y);
    gl_Position = vec4(p.x * view.x + view.z, p.y * view.y + view.w, 0.0, 1.0);
    v_uv = vec2(uv0.x * pos.x + uv0.z * pos.y + uv1.x,
                uv0.y * pos.x + uv0.w * pos.y + uv1.y);
    v_color = color;
}
