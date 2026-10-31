#version 450
layout(location = 0) in vec4 v_color;
layout(location = 1) in vec2 v_uv;
layout(location = 0) out vec4 out_color;

uniform vec4 mult;
uniform vec4 add;
uniform vec4 params; // kind (0 color, 1 bitmap, 2 gradient), gradient type, spread, focal
layout(binding = 0) uniform sampler2D tex;

void main() {
    vec4 c;
    if (params.x > 2.5) { // Stage3D layer: raw RGB, alpha ignored
        out_color = vec4(texture(tex, v_uv).rgb, 1.0);
        return;
    }
    if (params.x < 0.5) {
        c = v_color;
    } else if (params.x < 1.5) {
        c = texture(tex, v_uv);
    } else {
        float t;
        if (params.y < 0.5) {
            t = v_uv.x;
        } else if (params.y < 1.5) {
            t = length(v_uv * 2.0 - 1.0);
        } else {
            float focal = params.w;
            vec2 uv = v_uv * 2.0 - 1.0;
            vec2 d = vec2(focal, 0.0) - uv;
            float l = length(d);
            d /= l;
            t = l / (sqrt(1.0 - focal * focal * d.y * d.y) + focal * d.x);
        }
        if (params.z < 0.5) {
            t = clamp(t, 0.0, 1.0);
        } else if (params.z < 1.5) {
            t = fract(t);
        } else {
            t = mod(abs(t), 2.0);
            if (t > 1.0) t = 2.0 - t;
        }
        c = texture(tex, vec2(t * (255.0 / 256.0) + 0.5 / 256.0, 0.5));
    }
    if (c.a > 0.0) {
        c.rgb /= c.a;
        c = clamp(c * mult + add, 0.0, 1.0);
        c.rgb *= c.a;
    } else if (add.a > 0.0) {
        c = clamp(add, 0.0, 1.0);
        c.rgb *= c.a;
    }
    out_color = c;
}
