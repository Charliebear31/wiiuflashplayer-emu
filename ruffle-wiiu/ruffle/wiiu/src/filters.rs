//! CPU versions of Flash's bitmap filters (Ruffle only ships wgpu shader versions).
//! Images are premultiplied RGBA8, the same as the host's textures.

use ruffle_render::filters::Filter;
use std::sync::atomic::{AtomicU32, Ordering};

// Where drawing time goes, since the host last asked (rw_render_stats), for the console's log.
pub static STATS: [AtomicU32; 8] = [const { AtomicU32::new(0) }; 8];
pub const ST_FILTERS: usize = 0; // filters run on the CPU
pub const ST_FILTER_KPX: usize = 1; // thousands of pixels they covered
pub const ST_FILTER_US: usize = 2; // microseconds in them (with reading / writing the textures)
pub const ST_SHAPES: usize = 3; // shapes tessellated
pub const ST_SHAPE_US: usize = 4; // microseconds tessellating
pub const ST_UPLOAD_KB: usize = 5; // KB of texture updates (BitmapData changes)
pub const ST_OFFSCREEN: usize = 6; // draws into textures (BitmapData.draw)
pub const ST_GPU_FILTERS: usize = 7; // filters run on the GPU

pub fn stat_add(i: usize, v: u32) {
    STATS[i].fetch_add(v, Ordering::Relaxed);
}

/// Logs (once per kind) a filter that runs on the CPU, so the console's log shows what to move next.
pub fn note_cpu_filter(filter: &Filter) {
    static SEEN: AtomicU32 = AtomicU32::new(0);
    let (bit, name) = match filter {
        Filter::BlurFilter(_) => (0, "blur"),
        Filter::GlowFilter(_) => (1, "glow"),
        Filter::DropShadowFilter(_) => (2, "drop shadow"),
        Filter::ColorMatrixFilter(_) => (3, "colour matrix"),
        Filter::ConvolutionFilter(_) => (4, "convolution"),
        Filter::BevelFilter(_) => (5, "bevel"),
        Filter::GradientGlowFilter(_) => (6, "gradient glow"),
        Filter::GradientBevelFilter(_) => (7, "gradient bevel"),
        _ => (8, "other"),
    };
    if SEEN.fetch_or(1 << bit, Ordering::Relaxed) & (1 << bit) == 0 {
        crate::host::log(2, &format!("filter on the CPU: {name}"));
    }
}

pub fn count_cpu_work(pixels: u32, filters: u32) {
    stat_add(ST_FILTER_KPX, pixels.saturating_mul(filters) / 1000);
    stat_add(ST_FILTERS, filters);
}

/// Microseconds since the wasm clock's origin (for the stats).
pub fn now_us() -> u64 {
    std::time::SystemTime::now()
        .duration_since(std::time::UNIX_EPOCH)
        .map(|d| d.as_micros() as u64)
        .unwrap_or(0)
}

pub struct Image {
    pub w: usize,
    pub h: usize,
    pub px: Vec<u8>,
}

/// Filters this module implements (displacement maps and Pixel Bender shaders are not).
pub fn supported(filter: &Filter) -> bool {
    let _ = filter;
    true
}

/// The filters the host can run on the GPU, as (kind, parameters) for host_filter: 1 blur (blur x,
/// blur y, passes), 2 glow / drop shadow (blur x, blur y, passes, strength, offset x, offset y,
/// premultiplied r, g, b, a, inner, knockout, hide object), 3 colour matrix (the 20 values), 4 bevel
/// (blur x, blur y, passes, strength, offset x, offset y, premultiplied highlight then shadow colours,
/// inner, knockout, on top). Others return None (CPU only).
pub fn gpu_params(filter: &Filter) -> Option<(u32, Vec<f32>)> {
    let flag = |b: bool| if b { 1.0 } else { 0.0 };
    let color = |c: swf::Color| {
        let a = c.a as f32 / 255.0;
        [c.r as f32 / 255.0 * a, c.g as f32 / 255.0 * a, c.b as f32 / 255.0 * a, a]
    };
    match filter {
        Filter::BlurFilter(f) => Some((1, vec![f.blur_x.to_f32(), f.blur_y.to_f32(), f.num_passes().max(1) as f32])),
        Filter::GlowFilter(f) => {
            let c = color(f.color);
            Some((2, vec![
                f.blur_x.to_f32(), f.blur_y.to_f32(), f.num_passes().max(1) as f32, f.strength.to_f32(), 0.0, 0.0,
                c[0], c[1], c[2], c[3], flag(f.is_inner()), flag(f.is_knockout()), 0.0,
            ]))
        }
        Filter::DropShadowFilter(f) => {
            let (a, d) = (f.angle.to_f32(), f.distance.to_f32());
            let c = color(f.color);
            Some((2, vec![
                f.blur_x.to_f32(), f.blur_y.to_f32(), f.num_passes().max(1) as f32, f.strength.to_f32(),
                (a.cos() * d).round(), (a.sin() * d).round(), c[0], c[1], c[2], c[3], flag(f.is_inner()),
                flag(f.is_knockout()), flag(f.hide_object()),
            ]))
        }
        Filter::ColorMatrixFilter(f) => Some((3, f.matrix.to_vec())),
        Filter::BevelFilter(f) => {
            let (a, d) = (f.angle.to_f32(), f.distance.to_f32());
            let (h, s) = (color(f.highlight_color), color(f.shadow_color));
            Some((4, vec![
                f.blur_x.to_f32(), f.blur_y.to_f32(), f.num_passes().max(1) as f32, f.strength.to_f32(),
                (a.cos() * d).round(), (a.sin() * d).round(), h[0], h[1], h[2], h[3], s[0], s[1], s[2], s[3],
                flag(f.is_inner()), flag(f.is_knockout()), flag(f.is_on_top()),
            ]))
        }
        _ => None,
    }
}

pub fn apply(img: &mut Image, filter: &Filter) {
    match filter {
        Filter::BlurFilter(f) => {
            let passes = f.num_passes().max(1);
            let (w, h) = (img.w, img.h);
            blur_u8(&mut img.px, w, h, 4, f.blur_x.to_f32(), f.blur_y.to_f32(), passes);
        }
        Filter::ColorMatrixFilter(f) => color_matrix(img, &f.matrix),
        Filter::ConvolutionFilter(f) => convolution(img, f),
        Filter::GlowFilter(f) => {
            let layer = Layer {
                blur: (f.blur_x.to_f32(), f.blur_y.to_f32(), f.num_passes().max(1)),
                strength: f.strength.to_f32(),
                offset: (0.0, 0.0),
                inner: f.is_inner(),
            };
            glow_u8(img, &layer, f.color, f.is_knockout(), false);
        }
        Filter::DropShadowFilter(f) => {
            let (a, d) = (f.angle.to_f32(), f.distance.to_f32());
            let layer = Layer {
                blur: (f.blur_x.to_f32(), f.blur_y.to_f32(), f.num_passes().max(1)),
                strength: f.strength.to_f32(),
                offset: (a.cos() * d, a.sin() * d),
                inner: f.is_inner(),
            };
            glow_u8(img, &layer, f.color, f.is_knockout(), f.hide_object());
        }
        Filter::GradientGlowFilter(f) => {
            let (a, d) = (f.angle.to_f32(), f.distance.to_f32());
            let layer = Layer {
                blur: (f.blur_x.to_f32(), f.blur_y.to_f32(), f.num_passes().max(1)),
                strength: f.strength.to_f32(),
                offset: (a.cos() * d, a.sin() * d),
                inner: f.is_inner(),
            };
            let mask = shadow_mask(img, &layer);
            let ramp = gradient_ramp(&f.colors);
            let colored: Vec<[f32; 4]> = mask.iter().map(|&m| ramp[(m * 255.0) as usize]).collect();
            composite(img, &colored, f.is_inner(), f.is_knockout(), false);
        }
        Filter::BevelFilter(f) => {
            let (a, d) = (f.angle.to_f32(), f.distance.to_f32());
            let (hi, sh) = bevel_masks(img, a, d, f.blur_x.to_f32(), f.blur_y.to_f32(), f.num_passes().max(1), f.strength.to_f32());
            let (hc, sc) = (premul(f.highlight_color), premul(f.shadow_color));
            let colored: Vec<[f32; 4]> = hi
                .iter()
                .zip(&sh)
                .map(|(&h, &s)| {
                    // highlight over shadow
                    let mut o = [0.0; 4];
                    for k in 0..4 {
                        o[k] = hc[k] * h + sc[k] * s * (1.0 - hc[3] * h);
                    }
                    o
                })
                .collect();
            bevel_composite(img, colored, f.is_inner(), f.is_on_top(), f.is_knockout());
        }
        Filter::GradientBevelFilter(f) => {
            let (a, d) = (f.angle.to_f32(), f.distance.to_f32());
            let (hi, sh) = bevel_masks(img, a, d, f.blur_x.to_f32(), f.blur_y.to_f32(), f.num_passes().max(1), f.strength.to_f32());
            let ramp = gradient_ramp(&f.colors);
            // 0 = full shadow, 128 = flat, 255 = full highlight
            let colored: Vec<[f32; 4]> = hi
                .iter()
                .zip(&sh)
                .map(|(&h, &s)| ramp[((0.5 + (h - s) * 0.5).clamp(0.0, 1.0) * 255.0) as usize])
                .collect();
            bevel_composite(img, colored, f.is_inner(), f.is_on_top(), f.is_knockout());
        }
        Filter::DisplacementMapFilter(f) => displace(img, f),
        // Needs Pixel Bender programs, which this player can't run.
        Filter::ShaderFilter(_) => {}
    }
}

/// Each pixel comes from elsewhere in the picture, by how far the map's colour there is from middle grey.
fn displace(img: &mut Image, f: &ruffle_render::filters::DisplacementMapFilter) {
    use ruffle_render::filters::DisplacementMapFilterMode as Mode;
    let Some(map_handle) = &f.map_bitmap else { return };
    let map = crate::render::as_texture(map_handle);
    let (mw, mh) = (map.width as i32, map.height as i32);
    let mut map_px = vec![0u8; (mw * mh * 4) as usize];
    unsafe { crate::host::host_tex_read(map.id, 0, 0, mw as u32, mh as u32, map_px.as_mut_ptr()) }
    let component = |p: &[u8], which: u8| -> f32 {
        match which {
            1 => p[0] as f32,
            2 => p[1] as f32,
            4 => p[2] as f32,
            8 => p[3] as f32,
            _ => 128.0,
        }
    };
    let (w, h) = (img.w as i32, img.h as i32);
    let (sx, sy) = (f.scale_x * f.viewscale_x / 256.0, f.scale_y * f.viewscale_y / 256.0);
    let fill = premul(f.color).map(|v| (v * 255.0) as u8);
    let mut out = vec![0u8; img.px.len()];
    for y in 0..h {
        for x in 0..w {
            // The picture may be drawn larger than the map, which stays in the game's own pixels.
            let (mx, my) = ((x as f32 / f.viewscale_x) as i32 - f.map_point.0, (y as f32 / f.viewscale_y) as i32 - f.map_point.1);
            let (cx, cy) = if mx >= 0 && my >= 0 && mx < mw && my < mh {
                let p = &map_px[((my * mw + mx) * 4) as usize..][..4];
                (component(p, f.component_x), component(p, f.component_y))
            } else {
                (128.0, 128.0)
            };
            let (mut tx, mut ty) = (x + ((cx - 128.0) * sx) as i32, y + ((cy - 128.0) * sy) as i32);
            let inside = tx >= 0 && ty >= 0 && tx < w && ty < h;
            let o = ((y * w + x) * 4) as usize;
            if !inside {
                match f.mode {
                    Mode::Clamp => {
                        tx = tx.clamp(0, w - 1);
                        ty = ty.clamp(0, h - 1);
                    }
                    Mode::Wrap => {
                        tx = tx.rem_euclid(w);
                        ty = ty.rem_euclid(h);
                    }
                    Mode::Ignore => {
                        tx = x;
                        ty = y;
                    }
                    Mode::Color => {
                        out[o..o + 4].copy_from_slice(&fill);
                        continue;
                    }
                }
            }
            let s = ((ty * w + tx) * 4) as usize;
            out[o..o + 4].copy_from_slice(&img.px[s..s + 4]);
        }
    }
    img.px = out;
}

// ---------------------------------------------------------------- helpers

fn channel(img: &Image, c: usize) -> Vec<f32> {
    img.px.chunks_exact(4).map(|p| p[c] as f32 / 255.0).collect()
}

fn premul(c: swf::Color) -> [f32; 4] {
    let a = c.a as f32 / 255.0;
    [c.r as f32 / 255.0 * a, c.g as f32 / 255.0 * a, c.b as f32 / 255.0 * a, a]
}

// The blur for blur filters and glow / shadow / bevel masks: 8-bit, all channels in one go, integer sums
// (Flash's own blur is 8-bit too). It used to convert each channel to floats and back, one at a time,
// and a game running one blur a frame spent ~25 ms drawing in Cemu, several times that on the console.

/// One box blur pass along a line of `n` u8 samples `stride` apart, with Flash's fractional width.
fn box_line_u8(src: &[u8], dst: &mut [u8], n: usize, stride: usize, width: f32) {
    let r = ((width - 1.0) / 2.0).max(0.0);
    let ri = r.floor() as usize;
    let frac = ((r - ri as f32) * 256.0) as u64; // weight of the two edge samples, /256
    // out = (sum * 256 + edges * frac) / ((2r + 1) * 256), as a multiply by a 32.32 reciprocal
    let norm = ((1u64 << 32) as f64 / ((2.0 * r as f64 + 1.0) * 256.0)) as u64;
    let at = |i: usize| -> u64 { if i < n { src[i * stride] as u64 } else { 0 } };
    // Indexes are offset by ri + 1 so the window's left edge never goes below 0.
    let off = ri + 1;
    let get = |j: usize| -> u64 { if j >= off { at(j - off) } else { 0 } };
    let mut sum: u64 = (0..=ri.min(n.saturating_sub(1))).map(|i| at(i)).sum();
    for i in 0..n {
        let j = i + off;
        let edge = get(j - ri - 1) + get(j + ri + 1);
        let acc = sum * 256 + edge * frac;
        dst[i * stride] = (((acc * norm) + (1 << 31)) >> 32).min(255) as u8;
        sum = sum + get(j + ri + 1) - get(j - ri);
    }
}

/// Blurs `px` (w x h pixels of `ch` interleaved u8 channels) in place.
fn blur_u8(px: &mut [u8], w: usize, h: usize, ch: usize, bx: f32, by: f32, passes: u8) {
    if w == 0 || h == 0 {
        return;
    }
    let mut tmp = px.to_vec();
    for _ in 0..passes {
        if bx > 1.0 {
            for y in 0..h {
                for c in 0..ch {
                    let o = y * w * ch + c;
                    box_line_u8(&px[o..], &mut tmp[o..], w, ch, bx);
                }
            }
            px.copy_from_slice(&tmp);
        }
        if by > 1.0 {
            for x in 0..w {
                for c in 0..ch {
                    let o = x * ch + c;
                    box_line_u8(&px[o..], &mut tmp[o..], h, w * ch, by);
                }
            }
            px.copy_from_slice(&tmp);
        }
    }
}

/// x / 255 for x up to 255 * 255 * 2, without a divide.
#[inline(always)]
fn div255(x: u32) -> u32 {
    (x + 1 + (x >> 8)) >> 8
}

/// Glow and drop shadow in 8-bit integers, one pass over the pixels: the same result as shadow_mask +
/// tint + composite, which converted every pixel to floats several times and built two temporary
/// layers. (Those filters took ~18 ms each on 50 K pixels in Cemu; a game used one or two a frame.)
fn glow_u8(img: &mut Image, l: &Layer, color: swf::Color, knockout: bool, hide_object: bool) {
    let (w, h) = (img.w, img.h);
    let mut m: Vec<u8> = img.px.chunks_exact(4).map(|p| if l.inner { 255 - p[3] } else { p[3] }).collect();
    blur_u8(&mut m, w, h, 1, l.blur.0, l.blur.1, l.blur.2);
    // Shift by the offset, filling with "fully covered" for inner shadows, "nothing" for outer ones.
    let (ox, oy) = (l.offset.0.round() as isize, l.offset.1.round() as isize);
    if ox != 0 || oy != 0 {
        let fill = if l.inner { 255 } else { 0 };
        let mut out = vec![fill; m.len()];
        for y in 0..h as isize {
            let sy = y - oy;
            if sy < 0 || sy >= h as isize {
                continue;
            }
            let x0 = ox.max(0);
            let x1 = (w as isize + ox).min(w as isize);
            if x0 < x1 {
                let d = (y * w as isize + x0) as usize;
                let s = (sy * w as isize + x0 - ox) as usize;
                let n = (x1 - x0) as usize;
                out[d..d + n].copy_from_slice(&m[s..s + n]);
            }
        }
        m = out;
    }
    let strength = (l.strength.max(0.0) * 256.0).min(65535.0 * 256.0) as u32;
    let ca = color.a as u32;
    let c = [div255(color.r as u32 * ca), div255(color.g as u32 * ca), div255(color.b as u32 * ca), ca];
    for (p, &mv) in img.px.chunks_exact_mut(4).zip(&m) {
        let mut v = ((mv as u32 * strength) >> 8).min(255);
        if l.inner {
            v = div255(v * p[3] as u32);
        }
        let lay = [div255(c[0] * v), div255(c[1] * v), div255(c[2] * v), div255(c[3] * v)];
        let s = [p[0] as u32, p[1] as u32, p[2] as u32, p[3] as u32];
        for k in 0..4 {
            let o = if l.inner {
                if knockout { lay[k] } else { lay[k] + div255(s[k] * (255 - lay[3])) } // glow over the object
            } else if knockout {
                div255(lay[k] * (255 - s[3]))
            } else if hide_object {
                lay[k]
            } else {
                s[k] + div255(lay[k] * (255 - s[3])) // the object over its glow / shadow
            };
            p[k] = o.min(255) as u8;
        }
    }
}

/// The alpha channel (inverted for inner glows / shadows), blurred with blur_u8, as 0..1 floats.
fn blurred_alpha(img: &Image, invert: bool, bx: f32, by: f32, passes: u8) -> Vec<f32> {
    let mut a: Vec<u8> = img.px.chunks_exact(4).map(|p| if invert { 255 - p[3] } else { p[3] }).collect();
    blur_u8(&mut a, img.w, img.h, 1, bx, by, passes);
    a.iter().map(|&v| v as f32 / 255.0).collect()
}

struct Layer {
    blur: (f32, f32, u8),
    strength: f32,
    offset: (f32, f32),
    inner: bool,
}

/// Glow / shadow coverage 0..1: (inverted for inner) alpha, blurred, shifted, strengthened, masked.
fn shadow_mask(img: &Image, l: &Layer) -> Vec<f32> {
    let (w, h) = (img.w, img.h);
    let alpha = channel(img, 3);
    let m = blurred_alpha(img, l.inner, l.blur.0, l.blur.1, l.blur.2);
    let m = shift(&m, w, h, l.offset, if l.inner { 1.0 } else { 0.0 });
    m.iter()
        .zip(&alpha)
        .map(|(&v, &a)| {
            let v = (v * l.strength).clamp(0.0, 1.0);
            if l.inner { v * a } else { v }
        })
        .collect()
}

/// Moves an image by (dx, dy), filling uncovered pixels with `fill`.
fn shift(src: &[f32], w: usize, h: usize, (dx, dy): (f32, f32), fill: f32) -> Vec<f32> {
    let (ox, oy) = (dx.round() as isize, dy.round() as isize);
    if ox == 0 && oy == 0 {
        return src.to_vec();
    }
    let mut out = vec![fill; src.len()];
    for y in 0..h as isize {
        let sy = y - oy;
        if sy < 0 || sy >= h as isize {
            continue;
        }
        for x in 0..w as isize {
            let sx = x - ox;
            if sx >= 0 && sx < w as isize {
                out[(y * w as isize + x) as usize] = src[(sy * w as isize + sx) as usize];
            }
        }
    }
    out
}

/// Puts a premultiplied effect layer together with the image.
fn composite(img: &mut Image, layer: &[[f32; 4]], inner: bool, knockout: bool, hide_object: bool) {
    for (p, l) in img.px.chunks_exact_mut(4).zip(layer) {
        let s = [p[0] as f32 / 255.0, p[1] as f32 / 255.0, p[2] as f32 / 255.0, p[3] as f32 / 255.0];
        let o: [f32; 4] = if inner {
            if knockout {
                *l
            } else {
                // glow on top of the object (the layer is already inside it)
                std::array::from_fn(|k| l[k] + s[k] * (1.0 - l[3]))
            }
        } else if knockout {
            std::array::from_fn(|k| l[k] * (1.0 - s[3]))
        } else if hide_object {
            *l
        } else {
            // object on top of its glow / shadow
            std::array::from_fn(|k| s[k] + l[k] * (1.0 - s[3]))
        };
        for k in 0..4 {
            p[k] = (o[k].clamp(0.0, 1.0) * 255.0 + 0.5) as u8;
        }
    }
}

/// Highlight and shadow coverage for bevels: the blurred alpha compared with itself shifted both ways.
fn bevel_masks(img: &Image, angle: f32, dist: f32, bx: f32, by: f32, passes: u8, strength: f32) -> (Vec<f32>, Vec<f32>) {
    let (w, h) = (img.w, img.h);
    let a = blurred_alpha(img, false, bx, by, passes);
    let (dx, dy) = (angle.cos() * dist, angle.sin() * dist);
    let towards = shift(&a, w, h, (dx, dy), 0.0);
    let away = shift(&a, w, h, (-dx, -dy), 0.0);
    let hi = towards.iter().zip(&away).map(|(t, f)| ((f - t) * strength).clamp(0.0, 1.0)).collect();
    let sh = towards.iter().zip(&away).map(|(t, f)| ((t - f) * strength).clamp(0.0, 1.0)).collect();
    (hi, sh)
}

fn bevel_composite(img: &mut Image, mut layer: Vec<[f32; 4]>, inner: bool, full: bool, knockout: bool) {
    // inner bevels show inside the object, outer ones outside, "full" (on top) both.
    for (l, p) in layer.iter_mut().zip(img.px.chunks_exact(4)) {
        let a = p[3] as f32 / 255.0;
        let m = if full { 1.0 } else if inner { a } else { 1.0 - a };
        for v in l.iter_mut() {
            *v *= m;
        }
    }
    composite(img, &layer, inner || full, knockout, false);
}

/// 256-entry premultiplied ramp for gradient glow / bevel filters.
fn gradient_ramp(records: &[swf::GradientRecord]) -> Vec<[f32; 4]> {
    (0..256)
        .map(|i| {
            let t = i as f32;
            let (first, last) = match (records.first(), records.last()) {
                (Some(f), Some(l)) => (f, l),
                _ => return [0.0; 4],
            };
            if t <= first.ratio as f32 {
                return premul(first.color);
            }
            if t >= last.ratio as f32 {
                return premul(last.color);
            }
            for pair in records.windows(2) {
                let (r0, r1) = (pair[0].ratio as f32, pair[1].ratio as f32);
                if t >= r0 && t <= r1 {
                    let f = if r1 > r0 { (t - r0) / (r1 - r0) } else { 0.0 };
                    let (c0, c1) = (premul(pair[0].color), premul(pair[1].color));
                    return std::array::from_fn(|k| c0[k] + (c1[k] - c0[k]) * f);
                }
            }
            premul(last.color)
        })
        .collect()
}

fn color_matrix(img: &mut Image, m: &[f32; 20]) {
    for p in img.px.chunks_exact_mut(4) {
        let a = p[3] as f32;
        // The matrix works on straight (unpremultiplied) 0..255 values.
        let s = if a > 0.0 {
            [p[0] as f32 * 255.0 / a, p[1] as f32 * 255.0 / a, p[2] as f32 * 255.0 / a, a]
        } else {
            [0.0, 0.0, 0.0, 0.0]
        };
        let mut o = [0.0f32; 4];
        for (r, out) in o.iter_mut().enumerate() {
            let row = &m[r * 5..r * 5 + 5];
            *out = (row[0] * s[0] + row[1] * s[1] + row[2] * s[2] + row[3] * s[3] + row[4]).clamp(0.0, 255.0);
        }
        let na = o[3] / 255.0;
        for k in 0..3 {
            p[k] = (o[k] * na + 0.5) as u8;
        }
        p[3] = (o[3] + 0.5) as u8;
    }
}

fn convolution(img: &mut Image, f: &swf::ConvolutionFilter) {
    let (w, h) = (img.w as isize, img.h as isize);
    let (cols, rows) = (f.num_matrix_cols as isize, f.num_matrix_rows as isize);
    if cols == 0 || rows == 0 || f.matrix.len() < (cols * rows) as usize {
        return;
    }
    let div = if f.divisor == 0.0 { 1.0 } else { f.divisor };
    let src = img.px.clone();
    // Straight-alpha source pixel, clamped to the edge or the default color outside.
    let dc = f.default_color;
    let get = |x: isize, y: isize| -> [f32; 4] {
        let (x, y) = if f.is_clamped() { (x.clamp(0, w - 1), y.clamp(0, h - 1)) } else { (x, y) };
        if x < 0 || y < 0 || x >= w || y >= h {
            return [dc.r as f32, dc.g as f32, dc.b as f32, dc.a as f32];
        }
        let i = ((y * w + x) * 4) as usize;
        let a = src[i + 3] as f32;
        if a == 0.0 {
            return [0.0; 4];
        }
        [src[i] as f32 * 255.0 / a, src[i + 1] as f32 * 255.0 / a, src[i + 2] as f32 * 255.0 / a, a]
    };
    for y in 0..h {
        for x in 0..w {
            let mut acc = [0.0f32; 4];
            for j in 0..rows {
                for i in 0..cols {
                    let k = f.matrix[(j * cols + i) as usize];
                    let p = get(x + i - cols / 2, y + j - rows / 2);
                    for c in 0..4 {
                        acc[c] += p[c] * k;
                    }
                }
            }
            let own = get(x, y);
            let mut o = [0.0f32; 4];
            for c in 0..4 {
                o[c] = (acc[c] / div + f.bias).clamp(0.0, 255.0);
            }
            if f.is_preserve_alpha() {
                o[3] = own[3];
            }
            let i = ((y * w + x) * 4) as usize;
            let na = o[3] / 255.0;
            for c in 0..3 {
                img.px[i + c] = (o[c] * na + 0.5) as u8;
            }
            img.px[i + 3] = (o[3] + 0.5) as u8;
        }
    }
}
