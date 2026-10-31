#version 450
// Flash filters on the GPU (gfx_gx2.c: gfx_filter), drawn with flashplayer.vert. The same maths as the CPU
// versions in ruffle/wiiu/src/filters.rs. Textures are premultiplied; samples outside one read as 0.
layout(location = 0) in vec4 v_color;
layout(location = 1) in vec2 v_uv;
layout(location = 0) out vec4 out_color;

uniform vec4 fparams; // x: mode (0 copy, 1 blur pass, 2 glow / shadow, 3 colour matrix); blur: y taps each side, z edge weight, w 1 / width
uniform vec4 fstep;   // xy: one texel along the blur direction (uv); zw: glow / shadow offset (uv)
uniform vec4 fcolor;  // glow / shadow colour, premultiplied
uniform vec4 fflags;  // x strength, y inner, z knockout, w hide object
uniform vec4 fm0;     // colour matrix rows (r, g, b, a) on straight colours, and the offsets (/ 255)
uniform vec4 fm1;
uniform vec4 fm2;
uniform vec4 fm3;
uniform vec4 fm4;
layout(binding = 0) uniform sampler2D src;
layout(binding = 1) uniform sampler2D blurred;

void main() {
    if (fparams.x < -0.5) { // clear this area (mode -1)
        out_color = vec4(0.0);
        return;
    }
    if (fparams.x < 0.5) {
        out_color = textureLod(src, v_uv, 0.0);
        return;
    }
    if (fparams.x < 1.5) {
        // One box blur pass with Flash's fractional width: full weight up to y texels each side, z at the edges.
        int taps = int(fparams.y + 0.5);
        vec4 sum = textureLod(src, v_uv, 0.0);
        for (int i = 1; i <= taps; i++) {
            vec2 d = fstep.xy * float(i);
            sum += textureLod(src, v_uv + d, 0.0) + textureLod(src, v_uv - d, 0.0);
        }
        vec2 e = fstep.xy * float(taps + 1);
        sum += (textureLod(src, v_uv + e, 0.0) + textureLod(src, v_uv - e, 0.0)) * fparams.z;
        out_color = sum * fparams.w;
        return;
    }
    if (fparams.x > 4.5) {
        // A layer over what's behind it with a blend mode the fixed blender can't do (mode 5): src is the layer,
        // blurred the backdrop. fparams.y: 3 multiply, 5 lighten, 6 darken, 7 difference, 13 overlay, 14 hard light.
        vec4 s5 = textureLod(src, v_uv, 0.0);
        vec4 d5 = textureLod(blurred, v_uv, 0.0);
        vec3 cs = s5.a > 0.0 ? s5.rgb / s5.a : vec3(0.0);
        vec3 cb = d5.a > 0.0 ? d5.rgb / d5.a : vec3(0.0);
        vec3 b5;
        if (fparams.y < 3.5) {
            b5 = cs * cb;
        } else if (fparams.y < 5.5) {
            b5 = max(cs, cb);
        } else if (fparams.y < 6.5) {
            b5 = min(cs, cb);
        } else if (fparams.y < 7.5) {
            b5 = abs(cb - cs);
        } else {
            // Overlay is hard light with the two swapped.
            vec3 top = fparams.y < 13.5 ? cb : cs;
            vec3 low = fparams.y < 13.5 ? cs : cb;
            vec3 mult = 2.0 * low * top;
            vec3 scrn = 1.0 - 2.0 * (1.0 - low) * (1.0 - top);
            b5 = vec3(top.r < 0.5 ? mult.r : scrn.r, top.g < 0.5 ? mult.g : scrn.g, top.b < 0.5 ? mult.b : scrn.b);
        }
        out_color = vec4(s5.rgb * (1.0 - d5.a) + d5.rgb * (1.0 - s5.a) + b5 * s5.a * d5.a, s5.a + d5.a * (1.0 - s5.a));
        return;
    }
    if (fparams.x > 3.5) {
        // Bevel (mode 4): the blurred alpha compared with itself shifted both ways gives highlight and
        // shadow (fcolor, fm0); fflags: x strength, y inner, z knockout, w on top ("full").
        vec4 s4 = textureLod(src, v_uv, 0.0);
        float towards = textureLod(blurred, v_uv - fstep.zw, 0.0).a;
        float away = textureLod(blurred, v_uv + fstep.zw, 0.0).a;
        float hi = clamp((away - towards) * fflags.x, 0.0, 1.0);
        float sh = clamp((towards - away) * fflags.x, 0.0, 1.0);
        vec4 l4 = fcolor * hi + fm0 * sh * (1.0 - fcolor.a * hi);
        bool full = fflags.w > 0.5, in4 = fflags.y > 0.5;
        l4 *= full ? 1.0 : (in4 ? s4.a : 1.0 - s4.a);
        vec4 o4;
        if (in4 || full) o4 = fflags.z > 0.5 ? l4 : l4 + s4 * (1.0 - l4.a);
        else o4 = fflags.z > 0.5 ? l4 * (1.0 - s4.a) : s4 + l4 * (1.0 - s4.a);
        out_color = clamp(o4, 0.0, 1.0);
        return;
    }
    if (fparams.x > 2.5) {
        // Colour matrix (mode 3), on straight colours.
        vec4 c = textureLod(src, v_uv, 0.0);
        vec4 u = c.a > 0.0 ? vec4(c.rgb / c.a, c.a) : vec4(0.0);
        vec4 o = clamp(vec4(dot(fm0, u), dot(fm1, u), dot(fm2, u), dot(fm3, u)) + fm4, 0.0, 1.0);
        out_color = vec4(o.rgb * o.a, o.a);
        return;
    }
    // Glow / drop shadow (mode 2): the blurred alpha (shifted, strengthened) tinted, put together with the object.
    vec4 s = textureLod(src, v_uv, 0.0);
    float m = textureLod(blurred, v_uv - fstep.zw, 0.0).a;
    bool inner = fflags.y > 0.5;
    if (inner) m = 1.0 - m; // outside the picture counts as covered for inner ones, like the CPU version
    m = clamp(m * fflags.x, 0.0, 1.0);
    if (inner) m *= s.a;
    vec4 l = fcolor * m;
    vec4 o;
    if (inner) {
        o = fflags.z > 0.5 ? l : l + s * (1.0 - l.a);
    } else if (fflags.z > 0.5) {
        o = l * (1.0 - s.a);
    } else if (fflags.w > 0.5) {
        o = l;
    } else {
        o = s + l * (1.0 - s.a);
    }
    out_color = clamp(o, 0.0, 1.0);
}
