//! Ruffle render backend that turns shapes into triangle meshes and hands them to the C host
//! (GX2 on Wii U, OpenGL on the PC test host) as one command list per frame or offscreen draw.

use crate::filters;
use crate::host::{self, op, prim};
use ruffle_render::filters::Filter;
use ruffle_render::backend::{
    BitmapCacheEntry, Context3D, Context3DProfile, PixelBenderOutput, PixelBenderTarget,
    RenderBackend, ShapeHandle, ShapeHandleImpl, ViewportDimensions,
};
use ruffle_render::bitmap::{
    Bitmap, BitmapFormat, BitmapHandle, BitmapHandleImpl, BitmapSource, PixelRegion,
    PixelSnapping, RgbaBufRead, SyncHandle,
};
use ruffle_render::commands::{Command, CommandHandler, CommandList, RenderBlendMode};
use ruffle_render::error::Error;
use ruffle_render::matrix::Matrix;
use ruffle_render::pixel_bender::{PixelBenderShader, PixelBenderShaderHandle};
use ruffle_render::pixel_bender_support::{ImageInputTexture, PixelBenderShaderArgument};
use ruffle_render::quality::StageQuality;
use ruffle_render::shape_utils::{DistilledShape, GradientType};
use ruffle_render::tessellator::{
    DrawType as TessDrawType, Gradient as TessGradient, ShapeTessellator,
};
use ruffle_render::transform::Transform;
use std::borrow::Cow;
use std::num::NonZeroU32;
use std::sync::Arc;
use swf::{BlendMode, Color, Twips};

#[derive(Debug)]
pub(crate) struct Texture {
    pub(crate) id: u32,
    pub(crate) width: u32,
    pub(crate) height: u32,
}

impl Texture {
    /// A new transparent host texture (also usable as a render target).
    pub(crate) fn new_empty(width: u32, height: u32) -> Self {
        let id = unsafe { host::host_tex_create(width, height, std::ptr::null(), width * 4) };
        Self { id, width, height }
    }
}

impl Drop for Texture {
    fn drop(&mut self) {
        unsafe { host::host_tex_free(self.id) }
    }
}

impl BitmapHandleImpl for Texture {}

pub(crate) fn as_texture(handle: &BitmapHandle) -> &Texture {
    <dyn std::any::Any>::downcast_ref(&*handle.0).expect("bitmap handle from another backend")
}

#[derive(Debug)]
struct MeshPart {
    id: u32,
    num_indices: u32,
    num_mask_indices: u32,
    kind: PartKind,
}

#[derive(Debug)]
enum PartKind {
    Color,
    Bitmap {
        handle: Option<BitmapHandle>,
        uv: [f32; 6],
        smoothed: bool,
        repeating: bool,
    },
    Gradient {
        ramp: Texture,
        gradient_type: u32,
        spread: u32,
        focal: f32,
        uv: [f32; 6],
    },
}

#[derive(Debug)]
struct Mesh {
    parts: Vec<MeshPart>,
}

impl Drop for Mesh {
    fn drop(&mut self) {
        for part in &self.parts {
            unsafe { host::host_mesh_free(part.id) }
        }
    }
}

impl ShapeHandleImpl for Mesh {}

fn as_mesh(handle: &ShapeHandle) -> &Mesh {
    <dyn std::any::Any>::downcast_ref(&*handle.0).expect("shape handle from another backend")
}

#[derive(Debug)]
struct OffscreenSync {
    texture: u32,
    region: PixelRegion,
}

impl SyncHandle for OffscreenSync {}

/// Column-major 3x3 from the tessellator -> the 2x3 affine part the host shaders use.
fn uv_matrix(m: &[[f32; 3]; 3]) -> [f32; 6] {
    [m[0][0], m[0][1], m[1][0], m[1][1], m[2][0], m[2][1]]
}

fn pack_color(c: Color) -> u32 {
    // Premultiply like the GPU backends do so the host can always use ONE / ONE_MINUS_SRC_ALPHA.
    let a = c.a as u32;
    let pm = |v: u8| (v as u32 * a + 127) / 255;
    pm(c.r) << 24 | pm(c.g) << 16 | pm(c.b) << 8 | a
}

fn srgb_to_linear(c: f32) -> f32 {
    if c <= 0.04045 {
        c / 12.92
    } else {
        ((c + 0.055) / 1.055).powf(2.4)
    }
}

fn linear_to_srgb(c: f32) -> f32 {
    if c <= 0.0031308 {
        c * 12.92
    } else {
        1.055 * c.powf(1.0 / 2.4) - 0.055
    }
}

/// 256x1 premultiplied ramp; the host shader computes t per pixel and samples it.
fn gradient_ramp(gradient: &TessGradient) -> Texture {
    let records = &gradient.records;
    let linear = gradient.interpolation == swf::GradientInterpolation::LinearRgb;
    let conv = |c: Color| {
        let mut v = [c.r as f32 / 255.0, c.g as f32 / 255.0, c.b as f32 / 255.0, c.a as f32 / 255.0];
        if linear {
            for x in &mut v[..3] {
                *x = srgb_to_linear(*x);
            }
        }
        v
    };
    let mut data = vec![0u8; 256 * 4];
    for i in 0..256usize {
        let t = i as f32;
        let mut color = [0.0f32; 4];
        if let (Some(first), Some(last)) = (records.first(), records.last()) {
            color = if t <= first.ratio as f32 {
                conv(first.color)
            } else if t >= last.ratio as f32 {
                conv(last.color)
            } else {
                let mut out = conv(last.color);
                for pair in records.windows(2) {
                    let (r0, r1) = (pair[0].ratio as f32, pair[1].ratio as f32);
                    if t >= r0 && t <= r1 {
                        let f = if r1 > r0 { (t - r0) / (r1 - r0) } else { 0.0 };
                        let (c0, c1) = (conv(pair[0].color), conv(pair[1].color));
                        for k in 0..4 {
                            out[k] = c0[k] + (c1[k] - c0[k]) * f;
                        }
                        break;
                    }
                }
                out
            };
        }
        if linear {
            for x in &mut color[..3] {
                *x = linear_to_srgb(*x);
            }
        }
        let a = color[3].clamp(0.0, 1.0);
        for k in 0..3 {
            data[i * 4 + k] = (color[k].clamp(0.0, 1.0) * a * 255.0 + 0.5) as u8;
        }
        data[i * 4 + 3] = (a * 255.0 + 0.5) as u8;
    }
    let id = unsafe { host::host_tex_create(256, 1, data.as_ptr(), 256 * 4) };
    Texture {
        id,
        width: 256,
        height: 1,
    }
}

fn read_texture(id: u32, x: u32, y: u32, w: u32, h: u32) -> filters::Image {
    let mut px = vec![0u8; (w * h * 4) as usize];
    unsafe { host::host_tex_read(id, x, y, w, h, px.as_mut_ptr()) }
    filters::Image {
        w: w as usize,
        h: h as usize,
        px,
    }
}

fn write_texture(id: u32, x: u32, y: u32, img: &filters::Image) {
    unsafe {
        host::host_tex_update(id, x, y, img.w as u32, img.h as u32, img.px.as_ptr(), img.w as u32 * 4)
    }
}

#[derive(Clone, Copy, PartialEq)]
enum MaskState {
    NoMask,
    DrawMaskStencil,
    DrawMaskedContent,
    ClearMaskStencil,
}

const MAX_LAYERS: usize = 8;

/// A shader filter's arguments with its first image input set to the picture being filtered.
fn shader_args_with_source<'a>(args: &[PixelBenderShaderArgument<'a>], source: &BitmapHandle) -> Vec<PixelBenderShaderArgument<'a>> {
    let mut args = args.to_vec();
    for arg in &mut args {
        if let PixelBenderShaderArgument::ImageInput { texture, .. } = arg {
            *texture = Some(ImageInputTexture::Bitmap(source.clone()));
            break;
        }
    }
    args
}

pub struct HostRenderer {
    dimensions: ViewportDimensions,
    tessellator: ShapeTessellator,
    quad: u32,
    quad_outline: u32,
    // Per-command-list state, reset by `begin`.
    cmds: Vec<u32>,
    mask_state: MaskState,
    num_masks: u32,
    blend_stack: Vec<BlendMode>,
    /// The next render_bitmap is a Stage3D layer.
    raw_next_bitmap: bool,
    /// Size of the target being drawn, and the layers alpha masks draw into (kept: they're slow to make).
    target_size: (u32, u32),
    target_id: u32,
    layers: Vec<BitmapHandle>,
    next_layer: usize,
    /// Layers below this hold pictures still being drawn or queued further out.
    layer_base: usize,
}

impl HostRenderer {
    pub fn new(width: u32, height: u32) -> Self {
        let white = 0xffff_ffffu32;
        let verts: [u32; 12] = [
            0f32.to_bits(), 0f32.to_bits(), white,
            1f32.to_bits(), 0f32.to_bits(), white,
            1f32.to_bits(), 1f32.to_bits(), white,
            0f32.to_bits(), 1f32.to_bits(), white,
        ];
        let tri: [u32; 6] = [0, 1, 2, 0, 2, 3];
        let outline: [u32; 4] = [0, 1, 2, 3];
        let (quad, quad_outline) = unsafe {
            (
                host::host_mesh_create(verts.as_ptr(), 4, tri.as_ptr(), 6),
                host::host_mesh_create(verts.as_ptr(), 4, outline.as_ptr(), 4),
            )
        };
        Self {
            dimensions: ViewportDimensions {
                width,
                height,
                scale_factor: 1.0,
            },
            tessellator: ShapeTessellator::new(),
            quad,
            quad_outline,
            cmds: Vec::with_capacity(1 << 16),
            mask_state: MaskState::NoMask,
            num_masks: 0,
            blend_stack: Vec::new(),
            raw_next_bitmap: false,
            target_size: (width, height),
            target_id: 0,
            layers: Vec::new(),
            next_layer: 0,
            layer_base: 0,
        }
    }

    fn begin(&mut self, clear: Option<Color>) {
        self.cmds.clear();
        self.mask_state = MaskState::NoMask;
        self.num_masks = 0;
        self.blend_stack.clear();
        if let Some(c) = clear {
            self.cmds.push(op::CLEAR);
            for v in [c.r, c.g, c.b, c.a] {
                self.cmds.push((v as f32 / 255.0).to_bits());
            }
        }
    }

    /// A texture the size of the target for one use while drawing this target.
    fn apply_shader_filter(
        &mut self,
        filter: &ruffle_render::filters::ShaderFilter<'_>,
        source: &BitmapHandle,
        source_point: (u32, u32),
        source_size: (u32, u32),
        destination: &BitmapHandle,
        dest_point: (i32, i32),
    ) -> Option<Box<dyn SyncHandle>> {
        let (src, dst) = (as_texture(source), as_texture(destination));
        let (sx, sy) = source_point;
        // Flash ignores the rectangle's size here when it starts at the corner.
        let (sw, sh) = if (sx, sy) == (0, 0) { (src.width, src.height) } else { (source_size.0.max(1), source_size.1.max(1)) };
        let dst_id = dst.id;
        let region = PixelRegion::for_whole_size(dst.width, dst.height);
        // The input is the part of the source that's asked for, in a picture of its own.
        let cut = BitmapHandle(Arc::new(Texture::new_empty(sw, sh)));
        let cut_id = as_texture(&cut).id;
        self.begin(Some(Color { r: 0, g: 0, b: 0, a: 0 }));
        self.push_blend_raw(255);
        let mut at = Transform::default();
        at.matrix.tx = Twips::from_pixels(-(sx as f64));
        at.matrix.ty = Twips::from_pixels(-(sy as f64));
        self.render_bitmap(
            source.clone(),
            at,
            false,
            PixelSnapping::Never,
            PixelRegion::for_whole_size(src.width, src.height),
        );
        self.flush(cut_id);
        let result = BitmapHandle(Arc::new(Texture::new_empty(sw, sh)));
        let result_id = as_texture(&result).id;
        self.begin(Some(Color { r: 0, g: 0, b: 0, a: 0 }));
        self.flush(result_id);
        let args = shader_args_with_source(&filter.shader_args, &cut);
        crate::pixel_bender::run(&filter.shader, &args, result_id, true);
        // What comes out replaces what's in the destination there.
        self.begin(None);
        self.push_blend_raw(255);
        let mut put = Transform::default();
        put.matrix.tx = Twips::from_pixels(dest_point.0 as f64);
        put.matrix.ty = Twips::from_pixels(dest_point.1 as f64);
        self.render_bitmap(result, put, false, PixelSnapping::Never, PixelRegion::for_whole_size(sw, sh));
        self.flush(dst_id);
        Some(Box::new(OffscreenSync { texture: dst_id, region }))
    }

    /// Puts pixels the CPU made into a picture the GPU has drawn into, by drawing them there: writing to its memory
    /// directly isn't seen where the GPU keeps its own copy of a target (CPU filters came out unfiltered in Cemu).
    fn put_pixels(&mut self, id: u32, x: u32, y: u32, img: &filters::Image) {
        let (w, h) = (img.w as u32, img.h as u32);
        let temp = unsafe { host::host_tex_create(w, h, img.px.as_ptr(), w * 4) };
        let temp = BitmapHandle(Arc::new(Texture { id: temp, width: w, height: h }));
        self.begin(None);
        self.push_blend_raw(255);
        let mut at = Transform::default();
        at.matrix.tx = Twips::from_pixels(x as f64);
        at.matrix.ty = Twips::from_pixels(y as f64);
        // Kept until after the flush: dropping the last handle frees the picture, and the draw would find it gone.
        self.render_bitmap(temp.clone(), at, false, PixelSnapping::Never, PixelRegion::for_whole_size(w, h));
        self.flush(id);
    }

    fn push_blend_raw(&mut self, mode: u32) {
        self.cmds.extend([op::BLEND, mode]);
    }

    /// Makes room for `count` more layers. When they've run out, what's queued for this target is drawn now, which
    /// frees the layers it was using. False only when more than MAX_LAYERS are nested inside one another.
    fn make_room_for_layers(&mut self, count: usize) -> bool {
        if self.next_layer + count <= MAX_LAYERS {
            return true;
        }
        if self.layer_base + count > MAX_LAYERS {
            return false;
        }
        let target = self.target_id;
        self.flush(target);
        self.next_layer = self.layer_base;
        // Each list of commands starts with the mask and blend state reset.
        if !matches!(self.mask_state, MaskState::NoMask) {
            self.push_mask_state();
        }
        if let Some(&blend) = self.blend_stack.last()
            && blend != BlendMode::Normal
        {
            self.push_blend(blend);
        }
        true
    }

    fn layer(&mut self, w: u32, h: u32) -> Option<BitmapHandle> {
        let n = self.next_layer;
        if n >= MAX_LAYERS {
            return None;
        }
        self.next_layer += 1;
        if let Some(layer) = self.layers.get(n) {
            let t = as_texture(layer);
            if t.width == w && t.height == h {
                return Some(layer.clone());
            }
        }
        let layer = BitmapHandle(Arc::new(Texture::new_empty(w, h)));
        if n < self.layers.len() {
            self.layers[n] = layer.clone();
        } else {
            self.layers.push(layer.clone());
        }
        Some(layer)
    }

    /// Draws commands into a transparent layer, leaving what's queued for the current target as it was.
    fn draw_to_layer(&mut self, layer: &BitmapHandle, commands: CommandList) {
        let queued = std::mem::take(&mut self.cmds);
        let mask_state = std::mem::replace(&mut self.mask_state, MaskState::NoMask);
        let num_masks = self.num_masks;
        let blends = std::mem::take(&mut self.blend_stack);
        let target_id = self.target_id;
        let id = as_texture(layer).id;
        self.target_id = id;
        let outer_base = std::mem::replace(&mut self.layer_base, self.next_layer);
        self.begin(Some(Color { r: 0, g: 0, b: 0, a: 0 }));
        commands.execute(self);
        self.flush(id);
        // Everything inside it is in its picture now, so those layers are free again.
        self.next_layer = self.layer_base;
        self.layer_base = outer_base;
        self.target_id = target_id;
        self.cmds = queued;
        self.mask_state = mask_state;
        self.num_masks = num_masks;
        self.blend_stack = blends;
    }

    fn flush(&mut self, target: u32) {
        unsafe { host::host_frame(target, self.cmds.as_ptr(), self.cmds.len() as u32) }
        self.cmds.clear();
    }

    fn mask_state_number(&self) -> u32 {
        match self.mask_state {
            MaskState::NoMask => 0,
            MaskState::DrawMaskStencil => 1,
            MaskState::DrawMaskedContent => 2,
            MaskState::ClearMaskStencil => 3,
        }
    }

    fn push_mask_state(&mut self) {
        let state = self.mask_state_number();
        self.cmds.extend([op::MASK, state, self.num_masks]);
    }

    fn push_blend(&mut self, mode: BlendMode) {
        self.cmds.extend([op::BLEND, mode as u32]);
    }

    #[allow(clippy::too_many_arguments)]
    fn push_draw(
        &mut self,
        mesh: u32,
        count: u32,
        primitive: u32,
        matrix: &Matrix,
        mult: [f32; 4],
        add: [f32; 4],
    ) {
        self.cmds.extend([op::DRAW, mesh, 0, count, primitive]);
        for v in [
            matrix.a,
            matrix.b,
            matrix.c,
            matrix.d,
            matrix.tx.to_pixels() as f32,
            matrix.ty.to_pixels() as f32,
        ] {
            self.cmds.push(v.to_bits());
        }
        for v in mult.into_iter().chain(add) {
            self.cmds.push(v.to_bits());
        }
    }

    fn draw_quad(&mut self, color: Color, matrix: Matrix, primitive: u32) {
        let mult = [
            color.r as f32 / 255.0,
            color.g as f32 / 255.0,
            color.b as f32 / 255.0,
            color.a as f32 / 255.0,
        ];
        let (mesh, count) = match primitive {
            prim::TRIANGLES => (self.quad, 6),
            prim::LINES => (self.quad_outline, 2),
            _ => (self.quad_outline, 4),
        };
        self.push_draw(mesh, count, primitive, &matrix, mult, [0.0; 4]);
        self.cmds.push(0);
    }

    fn register_shape_internal(
        &mut self,
        shape: DistilledShape,
        bitmap_source: &dyn BitmapSource,
        scale: f32,
    ) -> Mesh {
        let t0 = filters::now_us();
        let lyon_mesh = self
            .tessellator
            .tessellate_shape_with_scale(shape, bitmap_source, scale);
        filters::stat_add(filters::ST_SHAPES, 1);
        filters::stat_add(filters::ST_SHAPE_US, (filters::now_us() - t0) as u32);
        let mut parts = Vec::with_capacity(lyon_mesh.draws.len());
        let mut words = Vec::new();
        for draw in lyon_mesh.draws {
            words.clear();
            for v in &draw.vertices {
                words.extend([v.x.to_bits(), v.y.to_bits(), pack_color(v.color)]);
            }
            let id = unsafe {
                host::host_mesh_create(
                    words.as_ptr(),
                    draw.vertices.len() as u32,
                    draw.indices.as_ptr(),
                    draw.indices.len() as u32,
                )
            };
            let kind = match draw.draw_type {
                TessDrawType::Color => PartKind::Color,
                TessDrawType::Bitmap(bitmap) => PartKind::Bitmap {
                    handle: bitmap_source.bitmap_handle(bitmap.bitmap_id, self),
                    uv: uv_matrix(&bitmap.matrix),
                    smoothed: bitmap.is_smoothed,
                    repeating: bitmap.is_repeating,
                },
                TessDrawType::Gradient { matrix, gradient } => {
                    let g = &lyon_mesh.gradients[gradient];
                    PartKind::Gradient {
                        ramp: gradient_ramp(g),
                        gradient_type: match g.gradient_type {
                            GradientType::Linear => 0,
                            GradientType::Radial => 1,
                            GradientType::Focal => 2,
                        },
                        spread: match g.repeat_mode {
                            swf::GradientSpread::Pad => 0,
                            swf::GradientSpread::Repeat => 1,
                            swf::GradientSpread::Reflect => 2,
                        },
                        focal: g.focal_point.to_f32().clamp(-0.98, 0.98),
                        uv: uv_matrix(&matrix),
                    }
                }
            };
            parts.push(MeshPart {
                id,
                num_indices: draw.indices.len() as u32,
                num_mask_indices: draw.mask_index_count,
                kind,
            });
        }
        Mesh { parts }
    }

    /// As RGBA, without another copy of the pixels when they already are (a game updating two full-screen
    /// bitmaps a frame copied about 4 MB a frame here for nothing).
    fn upload<'a>(&mut self, bitmap: Bitmap<'a>) -> (Bitmap<'a>, u32, u32) {
        let bitmap = match bitmap.format() {
            BitmapFormat::Rgba => bitmap,
            _ => bitmap.to_rgba(),
        };
        let (w, h) = (bitmap.width(), bitmap.height());
        (bitmap, w, h)
    }
}

impl RenderBackend for HostRenderer {
    fn viewport_dimensions(&self) -> ViewportDimensions {
        self.dimensions
    }

    fn set_viewport_dimensions(&mut self, dimensions: ViewportDimensions) {
        self.dimensions = dimensions;
    }

    fn register_shape(
        &mut self,
        shape: DistilledShape,
        bitmap_source: &dyn BitmapSource,
    ) -> ShapeHandle {
        self.register_shape_with_scale(shape, bitmap_source, 1.0)
    }

    fn register_shape_with_scale(
        &mut self,
        shape: DistilledShape,
        bitmap_source: &dyn BitmapSource,
        scale: f32,
    ) -> ShapeHandle {
        ShapeHandle(Arc::new(self.register_shape_internal(shape, bitmap_source, scale)))
    }

    fn render_offscreen(
        &mut self,
        handle: BitmapHandle,
        commands: CommandList,
        _quality: StageQuality,
        bounds: PixelRegion,
    ) -> Option<Box<dyn SyncHandle>> {
        let texture = as_texture(&handle).id;
        filters::stat_add(filters::ST_OFFSCREEN, 1);
        self.begin(None);
        let t = as_texture(&handle);
        self.target_size = (t.width, t.height);
        self.target_id = texture;
        self.next_layer = 0;
        self.layer_base = 0;
        commands.execute(self);
        self.flush(texture);
        Some(Box::new(OffscreenSync {
            texture,
            region: bounds,
        }))
    }

    fn is_offscreen_supported(&self) -> bool {
        true
    }

    fn is_filter_supported(&self, filter: &Filter) -> bool {
        filters::supported(filter)
    }

    fn apply_filter(
        &mut self,
        source: BitmapHandle,
        source_point: (u32, u32),
        source_size: (u32, u32),
        destination: BitmapHandle,
        dest_point: (i32, i32),
        filter: Filter,
    ) -> Option<Box<dyn SyncHandle>> {
        if !filters::supported(&filter) {
            return None;
        }
        if let Filter::ShaderFilter(shader_filter) = &filter {
            return self.apply_shader_filter(shader_filter, &source, source_point, source_size, &destination, dest_point);
        }
        let src = as_texture(&source);
        let dst = as_texture(&destination);
        let (sx, sy) = source_point;
        let sw = source_size.0.min(src.width.saturating_sub(sx));
        let sh = source_size.1.min(src.height.saturating_sub(sy));
        if sw == 0 || sh == 0 {
            return None;
        }
        let (dx, dy) = dest_point;
        if let Some((kind, p)) = filters::gpu_params(&filter) {
            let done = unsafe {
                host::host_filter(kind, src.id, sx, sy, sw, sh, dst.id, dx, dy, p.as_ptr(), p.len() as u32)
            };
            if done != 0 {
                filters::stat_add(filters::ST_GPU_FILTERS, 1);
                let (ox, oy) = ((-dx).max(0) as u32, (-dy).max(0) as u32);
                let (tx, ty) = (dx.max(0) as u32, dy.max(0) as u32);
                let cw = sw.saturating_sub(ox).min(dst.width.saturating_sub(tx));
                let ch = sh.saturating_sub(oy).min(dst.height.saturating_sub(ty));
                return Some(Box::new(OffscreenSync {
                    texture: dst.id,
                    region: PixelRegion::for_region(tx, ty, cw, ch),
                }));
            }
        }
        filters::note_cpu_filter(&filter);
        filters::count_cpu_work(sw * sh, 1);
        let t0 = filters::now_us();
        let mut img = read_texture(src.id, sx, sy, sw, sh);
        filters::apply(&mut img, &filter);
        // Clip the result into the destination.
        let (ox, oy) = ((-dx).max(0) as usize, (-dy).max(0) as usize);
        let (tx, ty) = (dx.max(0) as u32, dy.max(0) as u32);
        let cw = (img.w.saturating_sub(ox) as u32).min(dst.width.saturating_sub(tx));
        let ch = (img.h.saturating_sub(oy) as u32).min(dst.height.saturating_sub(ty));
        if cw == 0 || ch == 0 {
            return None;
        }
        let mut part = filters::Image {
            w: cw as usize,
            h: ch as usize,
            px: vec![0; (cw * ch * 4) as usize],
        };
        for row in 0..ch as usize {
            let from = ((oy + row) * img.w + ox) * 4;
            part.px[row * cw as usize * 4..(row + 1) * cw as usize * 4]
                .copy_from_slice(&img.px[from..from + cw as usize * 4]);
        }
        self.put_pixels(dst.id, tx, ty, &part);
        filters::stat_add(filters::ST_FILTER_US, (filters::now_us() - t0) as u32);
        Some(Box::new(OffscreenSync {
            texture: dst.id,
            region: PixelRegion::for_region(tx, ty, cw, ch),
        }))
    }

    fn submit_frame(
        &mut self,
        clear: Color,
        commands: CommandList,
        cache_entries: Vec<BitmapCacheEntry>,
    ) {
        // cacheAsBitmap: draw each cached object into its texture first, then run its filters on the CPU
        // (the texture already has room for what the filters spread out).
        for entry in cache_entries {
            let texture = as_texture(&entry.handle);
            let (id, w, h) = (texture.id, texture.width, texture.height);
            self.begin(Some(entry.clear));
            self.target_size = (w, h);
            self.target_id = id;
            self.next_layer = 0;
            self.layer_base = 0;
        self.layer_base = 0;
            entry.commands.execute(self);
            self.flush(id);
            let mut filters: Vec<_> = entry.filters.iter().filter(|f| !f.impotent()).collect();
            // On the GPU, in place, while the host can; whatever is left runs on the CPU (in order, so one
            // CPU-only filter takes the ones after it along).
            while let Some(&f) = filters.first() {
                if let Filter::ShaderFilter(shader_filter) = f {
                    let (result, source) = (Texture::new_empty(w, h), entry.handle.clone());
                    let args = shader_args_with_source(&shader_filter.shader_args, &source);
                    self.begin(Some(Color { r: 0, g: 0, b: 0, a: 0 }));
                    self.flush(result.id);
                    crate::pixel_bender::run(&shader_filter.shader, &args, result.id, true);
                    unsafe { host::host_tex_copy(id, result.id) };
                    filters.remove(0);
                    continue;
                }
                let Some((kind, p)) = filters::gpu_params(f) else { break };
                let done = unsafe { host::host_filter(kind, id, 0, 0, w, h, id, 0, 0, p.as_ptr(), p.len() as u32) };
                if done == 0 {
                    break;
                }
                filters::stat_add(filters::ST_GPU_FILTERS, 1);
                filters.remove(0);
            }
            if !filters.is_empty() {
                filters.iter().for_each(|f| filters::note_cpu_filter(f));
                filters::count_cpu_work(w * h, filters.len() as u32);
                let t0 = filters::now_us();
                let mut img = read_texture(id, 0, 0, w, h);
                for f in filters {
                    filters::apply(&mut img, f);
                }
                self.put_pixels(id, 0, 0, &img);
                filters::stat_add(filters::ST_FILTER_US, (filters::now_us() - t0) as u32);
            }
        }
        self.begin(Some(clear));
        self.target_size = (self.dimensions.width, self.dimensions.height);
        self.target_id = 0;
        self.next_layer = 0;
        self.layer_base = 0;
        commands.execute(self);
        self.flush(0);
    }

    fn create_empty_texture(
        &mut self,
        width: NonZeroU32,
        height: NonZeroU32,
    ) -> Result<BitmapHandle, Error> {
        let (w, h) = (width.get(), height.get());
        let id = unsafe { host::host_tex_create(w, h, std::ptr::null(), w * 4) };
        Ok(BitmapHandle(Arc::new(Texture {
            id,
            width: w,
            height: h,
        })))
    }

    fn register_bitmap(&mut self, bitmap: Bitmap<'_>) -> Result<BitmapHandle, Error> {
        let (bitmap, w, h) = self.upload(bitmap);
        let id = unsafe { host::host_tex_create(w, h, bitmap.data().as_ptr(), w * 4) };
        Ok(BitmapHandle(Arc::new(Texture {
            id,
            width: w,
            height: h,
        })))
    }

    fn update_texture(
        &mut self,
        handle: &BitmapHandle,
        bitmap: Bitmap<'_>,
        region: PixelRegion,
    ) -> Result<(), Error> {
        let texture = as_texture(handle);
        let (bitmap, w, _) = self.upload(bitmap);
        let mut region = region;
        region.clamp(texture.width.min(w), texture.height.min(bitmap.height()));
        if region.width() == 0 || region.height() == 0 {
            return Ok(());
        }
        let offset = (region.y_min * w + region.x_min) as usize * 4;
        filters::stat_add(filters::ST_UPLOAD_KB, region.width() * region.height() * 4 / 1024);
        unsafe {
            host::host_tex_update(
                texture.id,
                region.x_min,
                region.y_min,
                region.width(),
                region.height(),
                bitmap.data()[offset..].as_ptr(),
                w * 4,
            )
        }
        Ok(())
    }

    fn create_context3d(&mut self, profile: Context3DProfile) -> Result<Box<dyn Context3D>, Error> {
        Ok(Box::new(crate::context3d::HostContext3D::new(profile)))
    }

    fn debug_info(&self) -> Cow<'static, str> {
        Cow::Borrowed("Renderer: Wii U host")
    }

    fn name(&self) -> &'static str {
        "wiiu"
    }

    fn set_quality(&mut self, _quality: StageQuality) {}

    fn compile_pixelbender_shader(
        &mut self,
        shader: PixelBenderShader,
    ) -> Result<PixelBenderShaderHandle, Error> {
        Ok(crate::pixel_bender::compile(shader))
    }

    fn run_pixelbender_shader(
        &mut self,
        handle: PixelBenderShaderHandle,
        arguments: &[PixelBenderShaderArgument],
        target: &PixelBenderTarget,
    ) -> Result<PixelBenderOutput, Error> {
        match target {
            PixelBenderTarget::Bitmap(bitmap) => {
                let t = as_texture(bitmap);
                let (id, w, h) = (t.id, t.width, t.height);
                self.begin(Some(Color { r: 0, g: 0, b: 0, a: 0 }));
                self.flush(id);
                crate::pixel_bender::run(&handle, arguments, id, false);
                Ok(PixelBenderOutput::Bitmap(Box::new(OffscreenSync {
                    texture: id,
                    region: PixelRegion::for_whole_size(w, h),
                })))
            }
            PixelBenderTarget::Bytes { width, height } => {
                // Numbers out of a shader job, in a target of floats so nothing is rounded or clamped.
                let channels = handle.0.parsed_shader().output_channels().unwrap_or(4);
                let id = unsafe { host::host_tex_create_float_target(*width, *height) };
                let temp = Texture { id, width: *width, height: *height };
                crate::pixel_bender::run(&handle, arguments, id, false);
                let mut floats = vec![0f32; (*width * *height * 4) as usize];
                unsafe { host::host_tex_read_float(temp.id, *width, *height, floats.as_mut_ptr()) };
                let keep = if channels == 3 { 3 } else { 4 };
                let bytes = floats.chunks_exact(4).flat_map(|p| p[..keep].iter().flat_map(|v| v.to_le_bytes())).collect();
                Ok(PixelBenderOutput::Bytes(bytes))
            }
        }
    }

    fn resolve_sync_handle(
        &mut self,
        handle: Box<dyn SyncHandle>,
        with_rgba: RgbaBufRead,
    ) -> Result<(), Error> {
        let sync = <dyn std::any::Any>::downcast_ref::<OffscreenSync>(&*handle)
            .expect("sync handle from another backend");
        let (w, h) = (sync.region.width(), sync.region.height());
        let mut buf = vec![0u8; (w * h * 4) as usize];
        unsafe {
            host::host_tex_read(
                sync.texture,
                sync.region.x_min,
                sync.region.y_min,
                w,
                h,
                buf.as_mut_ptr(),
            )
        }
        with_rgba(&buf, w * 4);
        Ok(())
    }
}

impl CommandHandler for HostRenderer {
    fn render_bitmap(
        &mut self,
        bitmap: BitmapHandle,
        transform: Transform,
        smoothing: bool,
        pixel_snapping: PixelSnapping,
        region: PixelRegion,
    ) {
        let texture = as_texture(&bitmap);
        let (tw, th) = (texture.width as f32, texture.height as f32);
        let id = texture.id;
        let mut matrix = transform.matrix;
        pixel_snapping.apply(&mut matrix);
        matrix *= Matrix::scale(region.width() as f32, region.height() as f32);
        let quad = self.quad;
        self.push_draw(
            quad,
            6,
            prim::TRIANGLES,
            &matrix,
            transform.color_transform.mult_rgba_normalized(),
            transform.color_transform.add_rgba_normalized(),
        );
        let uv = [
            region.width() as f32 / tw,
            0.0,
            0.0,
            region.height() as f32 / th,
            region.x_min as f32 / tw,
            region.y_min as f32 / th,
        ];
        // kind 1: bitmap; kind 3: raw RGB with alpha 1 (Stage3D output, see render_stage3d)
        let kind = if std::mem::take(&mut self.raw_next_bitmap) { 3 } else { 1 };
        self.cmds.extend([kind, id, smoothing as u32]);
        self.cmds.extend(uv.map(f32::to_bits));
    }

    fn render_stage3d(&mut self, bitmap: BitmapHandle, transform: Transform) {
        let (w, h) = {
            let t = as_texture(&bitmap);
            (t.width, t.height)
        };
        // Stage3D output is copied over the stage as it is, without blending and ignoring its alpha
        // (as Flash and wgpu do).
        self.cmds.extend([op::BLEND, host::BLEND_OPAQUE]);
        self.raw_next_bitmap = true;
        self.render_bitmap(bitmap, transform, true, PixelSnapping::Never, PixelRegion::for_whole_size(w, h));
        let current = self.blend_stack.last().copied().unwrap_or(BlendMode::Normal);
        self.push_blend(current);
    }

    fn render_shape(&mut self, shape: ShapeHandle, transform: Transform) {
        let mult = transform.color_transform.mult_rgba_normalized();
        let add = transform.color_transform.add_rgba_normalized();
        let masking = matches!(
            self.mask_state,
            MaskState::DrawMaskStencil | MaskState::ClearMaskStencil
        );
        let mesh = as_mesh(&shape);
        for part in &mesh.parts {
            let count = if masking {
                part.num_mask_indices
            } else {
                part.num_indices
            };
            if count == 0 {
                continue;
            }
            // Payload first, so a bitmap without a handle is skipped before anything is emitted.
            let payload: Vec<u32> = match &part.kind {
                PartKind::Color => vec![0],
                PartKind::Bitmap {
                    handle,
                    uv,
                    smoothed,
                    repeating,
                } => {
                    let Some(handle) = handle else { continue };
                    let mut p = vec![
                        1,
                        as_texture(handle).id,
                        *smoothed as u32 | (*repeating as u32) << 1,
                    ];
                    p.extend(uv.map(f32::to_bits));
                    p
                }
                PartKind::Gradient {
                    ramp,
                    gradient_type,
                    spread,
                    focal,
                    uv,
                } => {
                    let mut p = vec![2, ramp.id, *gradient_type, *spread, focal.to_bits()];
                    p.extend(uv.map(f32::to_bits));
                    p
                }
            };
            self.push_draw(part.id, count, prim::TRIANGLES, &transform.matrix, mult, add);
            self.cmds.extend(payload);
        }
    }

    fn render_alpha_mask(&mut self, maskee_commands: CommandList, mask_commands: CommandList) {
        // The object and its mask each go into a layer of their own, the object's is multiplied by the mask's
        // alpha, and the result is drawn where the object would have been.
        let (w, h) = self.target_size;
        if !self.make_room_for_layers(2) {
            maskee_commands.execute(self);
            return;
        }
        let (Some(object), Some(mask)) = (self.layer(w, h), self.layer(w, h)) else {
            maskee_commands.execute(self);
            return;
        };
        self.draw_to_layer(&object, maskee_commands);
        self.draw_to_layer(&mask, mask_commands);

        let whole = PixelRegion::for_whole_size(w, h);
        let queued = std::mem::take(&mut self.cmds);
        let (mask_state, num_masks) = (std::mem::replace(&mut self.mask_state, MaskState::NoMask), self.num_masks);
        let blends = std::mem::take(&mut self.blend_stack);
        let target_id = self.target_id;
        let object_id = as_texture(&object).id;
        self.target_id = object_id;
        self.begin(None);
        self.push_blend(BlendMode::Alpha);
        self.render_bitmap(mask, Transform::default(), false, PixelSnapping::Never, whole);
        self.flush(object_id);
        self.target_id = target_id;
        self.cmds = queued;
        self.mask_state = mask_state;
        self.num_masks = num_masks;
        self.blend_stack = blends;
        self.render_bitmap(object, Transform::default(), false, PixelSnapping::Never, whole);
    }
    fn draw_rect(&mut self, color: Color, matrix: Matrix) {
        self.draw_quad(color, matrix, prim::TRIANGLES);
    }

    fn draw_line(&mut self, color: Color, mut matrix: Matrix) {
        matrix.tx += Twips::HALF_PX;
        matrix.ty += Twips::HALF_PX;
        self.draw_quad(color, matrix, prim::LINES);
    }

    fn draw_line_rect(&mut self, color: Color, mut matrix: Matrix) {
        matrix.tx += Twips::HALF_PX;
        matrix.ty += Twips::HALF_PX;
        self.draw_quad(color, matrix, prim::LINE_LOOP);
    }

    fn push_mask(&mut self) {
        self.num_masks += 1;
        self.mask_state = MaskState::DrawMaskStencil;
        self.push_mask_state();
    }

    fn activate_mask(&mut self) {
        self.mask_state = MaskState::DrawMaskedContent;
        self.push_mask_state();
    }

    fn deactivate_mask(&mut self) {
        self.mask_state = MaskState::ClearMaskStencil;
        self.push_mask_state();
    }

    fn pop_mask(&mut self) {
        self.num_masks -= 1;
        self.mask_state = if self.num_masks == 0 {
            MaskState::NoMask
        } else {
            MaskState::DrawMaskedContent
        };
        self.push_mask_state();
    }

    fn blend(&mut self, commands: CommandList, blend: RenderBlendMode) {
        let mode = match blend {
            RenderBlendMode::Builtin(mode) => mode,
            RenderBlendMode::Shader(shader) => {
                // The shader gets what's behind (background) and this group (foreground). Not under masks,
                // like the engine's other renderers.
                let (w, h) = self.target_size;
                if !matches!(self.mask_state, MaskState::NoMask) || self.num_masks > 0 {
                    commands.execute(self);
                    return;
                }
                if !self.make_room_for_layers(2) {
                    commands.execute(self);
                    return;
                }
                let (Some(layer), Some(behind)) = (self.layer(w, h), self.layer(w, h)) else {
                    commands.execute(self);
                    return;
                };
                self.draw_to_layer(&layer, commands);
                let target = self.target_id;
                self.flush(target);
                unsafe { host::host_tex_copy(as_texture(&behind).id, target) };
                let args = [
                    PixelBenderShaderArgument::ImageInput {
                        index: 0,
                        channels: 0xff,
                        name: "background".to_string(),
                        texture: Some(ImageInputTexture::Bitmap(behind)),
                    },
                    PixelBenderShaderArgument::ImageInput {
                        index: 1,
                        channels: 0xff,
                        name: "foreground".to_string(),
                        texture: Some(ImageInputTexture::Bitmap(layer)),
                    },
                ];
                crate::pixel_bender::run(&shader, &args, target, true);
                if let Some(&previous) = self.blend_stack.last()
                    && previous != BlendMode::Normal
                {
                    self.push_blend(previous);
                }
                return;
            }
        };
        let previous = self.blend_stack.last().copied().unwrap_or(BlendMode::Normal);
        // On a picture with see-through parts (not the screen) the blender's multiply, lighten and darken lose
        // what's drawn over nothing.
        let needs_backdrop = matches!(mode, BlendMode::Difference | BlendMode::Overlay | BlendMode::HardLight)
            || (self.target_id != 0 && matches!(mode, BlendMode::Multiply | BlendMode::Lighten | BlendMode::Darken));
        let one_draw = commands.commands.len() == 1
            && matches!(commands.commands[0], Command::RenderBitmap { .. } | Command::RenderShape { .. });
        // A mode applies to the group as a whole, so several things go through a layer. One thing on its own
        // doesn't need it for the modes the blender can do: games draw hundreds of additive particles a frame.
        if needs_backdrop || (mode != BlendMode::Normal && !one_draw) {
            let (w, h) = self.target_size;
            let room = self.make_room_for_layers(1);
            if let Some(layer) = if room { self.layer(w, h) } else { None } {
                self.draw_to_layer(&layer, commands);
                let whole = PixelRegion::for_whole_size(w, h);
                if needs_backdrop {
                    let target = self.target_id;
                    let state = self.mask_state_number();
                    self.flush(target);
                    let done = unsafe {
                        host::host_blend_layer(target, as_texture(&layer).id, mode as u32, state, self.num_masks)
                    };
                    // Each list of commands starts with the mask and blend state reset.
                    if state != 0 {
                        self.push_mask_state();
                    }
                    if previous != BlendMode::Normal {
                        self.push_blend(previous);
                    }
                    if done == 0 {
                        self.render_bitmap(layer, Transform::default(), false, PixelSnapping::Never, whole);
                    }
                } else {
                    let draw_mode = if mode == BlendMode::Layer { BlendMode::Normal } else { mode };
                    if draw_mode != previous {
                        self.push_blend(draw_mode);
                    }
                    self.render_bitmap(layer, Transform::default(), false, PixelSnapping::Never, whole);
                    if draw_mode != previous {
                        self.push_blend(previous);
                    }
                }
                return;
            }
        }
        self.blend_stack.push(mode);
        if mode != previous {
            self.push_blend(mode);
        }
        commands.execute(self);
        self.blend_stack.pop();
        if mode != previous {
            self.push_blend(previous);
        }
    }
}
