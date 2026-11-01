//! Stage3D (flash.display3D.Context3D) on the host GPU.
//!
//! AGAL shader pairs become GLSL through naga-agal and naga's GLSL writer (cached per vertex attribute
//! layout). Flash's pipeline state is kept here, and every drawTriangles is sent to the host as one
//! self-contained packet (`draw_words`), so the C side (OpenGL on PC, GX2 on Wii U) needs no state.

use crate::host;
use crate::render::{Texture as HostTexture, as_texture};
use naga_agal::{AgalError, SamplerConfig, VertexAttributeFormat};
use ruffle_render::backend::{
    BufferUsage, Context3D, Context3DBlendFactor, Context3DCommand, Context3DCompareMode,
    Context3DProfile, Context3DStencilAction, Context3DTextureFilter, Context3DTextureFormat,
    Context3DTriangleFace, Context3DVertexBufferFormat, Context3DWrapMode, IndexBuffer,
    ProgramType, ShaderModule, Texture, VertexBuffer,
};
use ruffle_render::bitmap::BitmapHandle;
use ruffle_render::error::Error;
use std::cell::RefCell;
use std::collections::HashMap;
use std::rc::Rc;
use std::sync::Arc;

const MAX_ATTRIBUTES: usize = 8;
const MAX_TEXTURES: usize = 8;
const VERTEX_CONSTANTS: usize = 128;
const FRAGMENT_CONSTANTS: usize = 64; // 28 in the baseline profiles, 64 in "standard"

// ---------------------------------------------------------------- resources

struct HostBuffer {
    id: u32,
    size: usize,
}

impl Drop for HostBuffer {
    fn drop(&mut self) {
        unsafe { host::host_c3d_buf_free(self.id) }
    }
}

impl HostBuffer {
    fn new(size: usize, vertex: bool) -> Self {
        let id = unsafe { host::host_c3d_buf_create(size.max(4) as u32, vertex as u32) };
        Self { id, size }
    }

    fn upload(&self, offset: usize, data: &[u8]) {
        if offset >= self.size {
            return;
        }
        let len = data.len().min(self.size - offset);
        unsafe { host::host_c3d_buf_upload(self.id, offset as u32, data.as_ptr(), len as u32) }
    }
}

struct C3dIndexBuffer(HostBuffer);
impl IndexBuffer for C3dIndexBuffer {}

struct C3dVertexBuffer {
    buf: HostBuffer,
    data32_per_vertex: u8,
}
impl VertexBuffer for C3dVertexBuffer {}

#[derive(Debug)]
struct C3dTexture {
    tex: Rc<HostTexture>,
    cube: bool,
    /// CompressedAlpha textures get DXT5 blocks (raw ATF), decoded to RGBA on upload.
    dxt5: bool,
    /// Compressed textures can get raw DXT1 blocks, decoded the same way.
    dxt1: bool,
}

impl Texture for C3dTexture {
    fn width(&self) -> u32 {
        self.tex.width
    }
    fn height(&self) -> u32 {
        // A cube texture's host texture holds its 6 faces stacked vertically.
        if self.cube { self.tex.width } else { self.tex.height }
    }
}

/// A Context3D.Program: the two AGAL shaders and their GLSL builds per vertex attribute layout.
struct ShaderPair {
    vertex: naga_agal::ParsedBytecode,
    fragment: naga_agal::ParsedBytecode,
    fragment_samplers: [Option<SamplerConfig>; MAX_TEXTURES],
    programs: RefCell<HashMap<[Option<VertexAttributeFormat>; MAX_ATTRIBUTES], u32>>,
}

impl ShaderModule for ShaderPair {}

impl Drop for ShaderPair {
    fn drop(&mut self) {
        for id in self.programs.borrow().values() {
            if *id != 0 {
                unsafe { host::host_c3d_program_free(*id) }
            }
        }
    }
}

fn to_glsl(module: &naga::Module, stage: naga::ShaderStage, flags: u32) -> Result<String, String> {
    use naga::back::glsl;
    let info = naga::valid::Validator::new(naga::valid::ValidationFlags::all(), naga::valid::Capabilities::all())
        .validate(module)
        .map_err(|e| format!("{e:?}"))?;
    let mut binding_map = glsl::BindingMap::default();
    binding_map.insert(naga::ResourceBinding { group: 0, binding: 0 }, 0);
    binding_map.insert(naga::ResourceBinding { group: 0, binding: 1 }, 1);
    for i in 0..MAX_TEXTURES as u32 {
        binding_map.insert(naga::ResourceBinding { group: 0, binding: naga_agal::TEXTURE_START_BIND_INDEX + i }, i as u8);
        binding_map.insert(naga::ResourceBinding { group: 0, binding: naga_agal::TEXTURE_SAMPLER_START_BIND_INDEX + i }, i as u8);
    }
    let options = glsl::Options {
        version: glsl::Version::Desktop(420),
        writer_flags: if flags & 1 != 0 { glsl::WriterFlags::ADJUST_COORDINATE_SPACE } else { glsl::WriterFlags::empty() },
        binding_map,
        zero_initialize_workgroup_memory: false,
    };
    let pipeline = glsl::PipelineOptions {
        shader_stage: stage,
        entry_point: naga_agal::SHADER_ENTRY_POINT.to_string(),
        multiview: None,
    };
    let mut out = String::new();
    let mut writer = glsl::Writer::new(&mut out, module, &info, &options, &pipeline, naga::proc::BoundsCheckPolicies::default())
        .map_err(|e| format!("{e:?}"))?;
    writer.write().map_err(|e| format!("{e:?}"))?;
    if out.contains("samplerCube") {
        out = cube_samplers_to_atlas(&out);
    }
    if flags & 2 != 0 {
        out = blocks_to_plain_uniforms(&out);
    }
    Ok(out)
}

/// Cube textures are 2D textures with the 6 faces stacked vertically (+X, -X, +Y, -Y, +Z, -Z), so
/// neither host needs cube map support: `samplerCube` becomes `sampler2D` and its `texture(s, dir)`
/// calls go through `cube_atlas`, which picks the face like the GPU would (D3D/GL face table) and
/// keeps the lookup half a texel inside that face so filtering doesn't bleed into the next one.
const CUBE_ATLAS_GLSL: &str = "
vec4 cube_atlas(sampler2D t, vec3 d) {
    vec3 a = abs(d);
    float face;
    float ma;
    vec2 st;
    if (a.x >= a.y && a.x >= a.z) {
        ma = a.x;
        if (d.x > 0.0) { face = 0.0; st = vec2(-d.z, -d.y); } else { face = 1.0; st = vec2(d.z, -d.y); }
    } else if (a.y >= a.z) {
        ma = a.y;
        if (d.y > 0.0) { face = 2.0; st = vec2(d.x, d.z); } else { face = 3.0; st = vec2(d.x, -d.z); }
    } else {
        ma = a.z;
        if (d.z > 0.0) { face = 4.0; st = vec2(d.x, -d.y); } else { face = 5.0; st = vec2(-d.x, -d.y); }
    }
    float half_texel = 0.5 / float(textureSize(t, 0).x);
    vec2 uv = clamp((st / max(ma, 1e-20) + 1.0) * 0.5, vec2(half_texel), vec2(1.0 - half_texel));
    return texture(t, vec2(uv.x, (face + uv.y) / 6.0));
}
vec4 cube_atlas(sampler2D t, vec3 d, float bias) { return cube_atlas(t, d); }
";

fn cube_samplers_to_atlas(src: &str) -> String {
    let mut names = Vec::new();
    let mut out = String::with_capacity(src.len() + CUBE_ATLAS_GLSL.len());
    let mut helper_added = false;
    for line in src.lines() {
        if let Some(pos) = line.find("uniform samplerCube ") {
            let name = line[pos + "uniform samplerCube ".len()..].trim_end_matches(';').trim();
            names.push(name.to_string());
            out.push_str(&line.replacen("samplerCube", "sampler2D", 1));
            out.push('\n');
            if !helper_added {
                out.push_str(CUBE_ATLAS_GLSL);
                helper_added = true;
            }
            continue;
        }
        out.push_str(line);
        out.push('\n');
    }
    for name in names {
        out = out.replace(&format!("texture({name},"), &format!("cube_atlas({name},"));
        out = out.replace(&format!("textureLod({name},"), &format!("cube_atlas({name},"));
    }
    out
}

/// `layout(std140, binding = N) uniform Block { vec4 name[K]; };` -> `uniform vec4 name[K];`
/// (GX2's register uniform mode has no blocks; the member keeps its name, so the code still works).
pub(crate) fn blocks_to_plain_uniforms(src: &str) -> String {
    let mut out = String::with_capacity(src.len());
    let mut lines = src.lines().peekable();
    while let Some(line) = lines.next() {
        let t = line.trim();
        if t.contains("uniform") && t.contains('{') && !t.contains("sampler") {
            let mut members = Vec::new();
            let open = t.find('{').unwrap();
            if let Some(close) = t.rfind('}') {
                // naga writes small blocks on one line: `... uniform Block { vec4 name[K]; };`
                members.extend(t[open + 1..close].split(';').map(str::trim).filter(|m| !m.is_empty()).map(|m| format!("{m};")));
            } else {
                for inner in lines.by_ref() {
                    let it = inner.trim();
                    if it.starts_with('}') {
                        break;
                    }
                    members.push(it.to_string());
                }
            }
            for m in members {
                out.push_str("uniform ");
                out.push_str(&m);
                out.push('\n');
            }
            continue;
        }
        out.push_str(line);
        out.push('\n');
    }
    out
}

impl ShaderPair {
    fn program(&self, attrs: &[Option<VertexAttributeFormat>; MAX_ATTRIBUTES]) -> Result<u32, naga_agal::Error> {
        if let Some(id) = self.programs.borrow().get(attrs) {
            return Ok(*id);
        }
        let flags = unsafe { host::host_c3d_flags() };
        let vs = naga_agal::agal_to_naga(&self.vertex, attrs)?;
        let fs = naga_agal::agal_to_naga(&self.fragment, attrs)?;
        let glsl = (to_glsl(&vs, naga::ShaderStage::Vertex, flags), to_glsl(&fs, naga::ShaderStage::Fragment, flags));
        let id = match glsl {
            (Ok(vs), Ok(fs)) => unsafe { host::host_c3d_program(vs.as_ptr(), vs.len() as u32, fs.as_ptr(), fs.len() as u32) },
            (Err(e), _) | (_, Err(e)) => {
                host::log(0, &format!("Stage3D: AGAL to GLSL failed: {e}"));
                0
            }
        };
        self.programs.borrow_mut().insert(*attrs, id);
        Ok(id)
    }
}

// ---------------------------------------------------------------- state

#[derive(Clone)]
struct Attribute {
    buffer: Rc<C3dVertexBuffer>,
    offset_words: u32,
    format: Context3DVertexBufferFormat,
}

#[derive(Clone)]
struct BoundTexture {
    texture: Rc<dyn Texture>,
}

struct Target {
    tex: u32,
    w: u32,
    h: u32,
    depth: bool,
}

pub struct HostContext3D {
    profile: Context3DProfile,
    front: Option<BitmapHandle>,
    back: Option<BitmapHandle>,
    back_size: Option<(u32, u32)>,
    back_depth: bool,
    /// Render-to-texture target, if any (else the back buffer).
    rtt: Option<(Rc<dyn Texture>, bool)>,
    cleared: bool,

    shaders: Option<Rc<ShaderPair>>,
    samplers: [SamplerConfig; MAX_TEXTURES],
    textures: [Option<BoundTexture>; MAX_TEXTURES],
    attributes: [Option<Attribute>; MAX_ATTRIBUTES],
    vertex_constants: Vec<f32>,
    fragment_constants: Vec<f32>,
    blend: (u32, u32),
    depth_mask: bool,
    depth_compare: u32,
    culling: u32,
    color_mask: u32,
    scissor: Option<[u32; 4]>,
    stencil: [u32; 8], // face, compare, both_pass, depth_fail, depth_pass_stencil_fail, ref, read, write
}

impl HostContext3D {
    pub fn new(profile: Context3DProfile) -> Self {
        Self {
            profile,
            front: None,
            back: None,
            back_size: None,
            back_depth: false,
            rtt: None,
            cleared: false,
            shaders: None,
            samplers: [SamplerConfig::default(); MAX_TEXTURES],
            textures: Default::default(),
            attributes: Default::default(),
            vertex_constants: vec![0.0; VERTEX_CONSTANTS * 4],
            fragment_constants: vec![0.0; FRAGMENT_CONSTANTS * 4],
            blend: (factor(Context3DBlendFactor::One), factor(Context3DBlendFactor::Zero)),
            depth_mask: true,
            depth_compare: compare(Context3DCompareMode::Less),
            culling: 0,
            color_mask: 0xf,
            scissor: None,
            stencil: [0, compare(Context3DCompareMode::Always), 0, 0, 0, 0, 0xff, 0xff],
        }
    }

    fn target(&self) -> Option<Target> {
        if let Some((tex, depth)) = &self.rtt {
            let t = <dyn std::any::Any>::downcast_ref::<C3dTexture>(tex.as_ref())?;
            return Some(Target { tex: t.tex.id, w: t.tex.width, h: t.tex.height, depth: *depth });
        }
        let back = as_texture(self.back.as_ref()?);
        Some(Target { tex: back.id, w: back.width, h: back.height, depth: self.back_depth })
    }

    fn draw(&mut self, index_buffer: &dyn IndexBuffer, first_index: usize, num_triangles: isize) {
        let Some(target) = self.target() else { return };
        let Some(shaders) = self.shaders.clone() else { return };
        let ib = <dyn std::any::Any>::downcast_ref::<C3dIndexBuffer>(index_buffer).expect("index buffer");
        // Flash skips draws whose program samples a texture that isn't bound.
        for (i, s) in shaders.fragment_samplers.iter().enumerate() {
            if s.is_some() && self.textures[i].is_none() {
                return;
            }
        }
        let formats = self.attributes.clone().map(|a| {
            a.map(|a| match a.format {
                Context3DVertexBufferFormat::Float1 => VertexAttributeFormat::Float1,
                Context3DVertexBufferFormat::Float2 => VertexAttributeFormat::Float2,
                Context3DVertexBufferFormat::Float3 => VertexAttributeFormat::Float3,
                Context3DVertexBufferFormat::Float4 => VertexAttributeFormat::Float4,
                Context3DVertexBufferFormat::Bytes4 => VertexAttributeFormat::Bytes4,
            })
        });
        let program = match shaders.program(&formats) {
            Ok(0) => return,
            Ok(id) => id,
            // Flash skips draws whose vertex shader reads an attribute without a buffer.
            Err(naga_agal::Error::MissingVertexAttributeData) => return,
            Err(e) => {
                host::log(0, &format!("Stage3D: AGAL compile failed: {e:?}"));
                return;
            }
        };
        let count = if num_triangles < 0 {
            (ib.0.size / 2).saturating_sub(first_index) as u32 / 3 * 3
        } else {
            num_triangles as u32 * 3
        };

        let mut w: Vec<u32> = Vec::with_capacity(96 + (VERTEX_CONSTANTS + FRAGMENT_CONSTANTS) * 4);
        w.extend([target.tex, target.w, target.h, target.depth as u32, program, ib.0.id, first_index as u32, count]);
        w.extend([self.blend.0, self.blend.1, self.depth_mask as u32, self.depth_compare, self.culling, self.color_mask]);
        match self.scissor {
            Some(s) => w.extend([1, s[0], s[1], s[2], s[3]]),
            None => w.extend([0, 0, 0, 0, 0]),
        }
        w.extend(self.stencil);
        for a in &self.attributes {
            match a {
                Some(a) => w.extend([
                    a.buffer.buf.id,
                    a.offset_words * 4,
                    a.format as u32,
                    a.buffer.data32_per_vertex as u32 * 4,
                ]),
                None => w.extend([0, 0, 0, 0]),
            }
        }
        for (i, t) in self.textures.iter().enumerate() {
            let s = self.samplers[i];
            match t.as_ref().and_then(|t| <dyn std::any::Any>::downcast_ref::<C3dTexture>(t.texture.as_ref())) {
                Some(c) => w.extend([c.tex.id, c.cube as u32, s.wrapping as u32, (s.filter as u32 != 0) as u32 | (s.mipmap as u32) << 1]),
                None => w.extend([0, 0, 0, 0]),
            }
        }
        w.extend(self.vertex_constants.iter().map(|f| f.to_bits()));
        w.extend(self.fragment_constants.iter().map(|f| f.to_bits()));
        unsafe { host::host_c3d_draw(w.as_ptr(), w.len() as u32) }
    }
}

fn factor(f: Context3DBlendFactor) -> u32 {
    match f {
        Context3DBlendFactor::Zero => 0,
        Context3DBlendFactor::One => 1,
        Context3DBlendFactor::SourceColor => 2,
        Context3DBlendFactor::OneMinusSourceColor => 3,
        Context3DBlendFactor::SourceAlpha => 4,
        Context3DBlendFactor::OneMinusSourceAlpha => 5,
        Context3DBlendFactor::DestinationAlpha => 6,
        Context3DBlendFactor::OneMinusDestinationAlpha => 7,
        Context3DBlendFactor::DestinationColor => 8,
        Context3DBlendFactor::OneMinusDestinationColor => 9,
    }
}

fn compare(c: Context3DCompareMode) -> u32 {
    match c {
        Context3DCompareMode::Never => 0,
        Context3DCompareMode::Less => 1,
        Context3DCompareMode::Equal => 2,
        Context3DCompareMode::LessEqual => 3,
        Context3DCompareMode::Greater => 4,
        Context3DCompareMode::NotEqual => 5,
        Context3DCompareMode::GreaterEqual => 6,
        Context3DCompareMode::Always => 7,
    }
}

fn stencil_op(a: Context3DStencilAction) -> u32 {
    match a {
        Context3DStencilAction::Keep => 0,
        Context3DStencilAction::Zero => 1,
        Context3DStencilAction::Set => 2,
        Context3DStencilAction::IncrementSaturate => 3,
        Context3DStencilAction::DecrementSaturate => 4,
        Context3DStencilAction::Invert => 5,
        Context3DStencilAction::IncrementWrap => 6,
        Context3DStencilAction::DecrementWrap => 7,
    }
}

fn face(f: Context3DTriangleFace) -> u32 {
    match f {
        Context3DTriangleFace::None => 0,
        Context3DTriangleFace::Back => 1,
        Context3DTriangleFace::Front => 2,
        Context3DTriangleFace::FrontAndBack => 3,
    }
}

/// Texture upload data as RGBA8 (Flash's packed formats are expanded; compressed ones aren't supported).
fn to_rgba(format: Context3DTextureFormat, src: &[u8], w: u32, h: u32) -> Option<Vec<u8>> {
    let n = (w * h) as usize;
    match format {
        Context3DTextureFormat::Bgra if src.len() >= n * 4 => Some(src[..n * 4].to_vec()),
        Context3DTextureFormat::BgraPacked if src.len() >= n * 2 => Some(
            src.chunks_exact(2)
                .take(n)
                .flat_map(|p| {
                    let v = u16::from_le_bytes([p[0], p[1]]);
                    let c = |s: u16| (((v >> s) & 0xf) * 17) as u8;
                    [c(8), c(4), c(0), c(12)]
                })
                .collect(),
        ),
        Context3DTextureFormat::BgrPacked if src.len() >= n * 2 => Some(
            src.chunks_exact(2)
                .take(n)
                .flat_map(|p| {
                    let v = u16::from_le_bytes([p[0], p[1]]);
                    [((v >> 11) * 255 / 31) as u8, (((v >> 5) & 63) * 255 / 63) as u8, ((v & 31) * 255 / 31) as u8, 255]
                })
                .collect(),
        ),
        _ => None,
    }
}

/// DXT1 (BC1) blocks -> RGBA8. Each 4x4 block is 8 bytes; color0 <= color1 picks the mode with a transparent entry.
fn dxt1_to_rgba(src: &[u8], w: u32, h: u32) -> Option<Vec<u8>> {
    let (bw, bh) = (w.div_ceil(4) as usize, h.div_ceil(4) as usize);
    if src.len() != bw * bh * 8 {
        return None;
    }
    let (w, h) = (w as usize, h as usize);
    let mut out = vec![0u8; w * h * 4];
    for by in 0..bh {
        for bx in 0..bw {
            let b = &src[(by * bw + bx) * 8..][..8];
            let c = |v: u16| [((v >> 11) * 255 / 31) as u32, (((v >> 5) & 63) * 255 / 63) as u32, ((v & 31) * 255 / 31) as u32];
            let (v0, v1) = (u16::from_le_bytes([b[0], b[1]]), u16::from_le_bytes([b[2], b[3]]));
            let (c0, c1) = (c(v0), c(v1));
            let colors = if v0 > v1 {
                [c0, c1, [0, 1, 2].map(|k| (2 * c0[k] + c1[k]) / 3), [0, 1, 2].map(|k| (c0[k] + 2 * c1[k]) / 3)]
            } else {
                [c0, c1, [0, 1, 2].map(|k| (c0[k] + c1[k]) / 2), [0, 0, 0]]
            };
            let bits = u32::from_le_bytes([b[4], b[5], b[6], b[7]]);
            for py in 0..4 {
                for px in 0..4 {
                    let (x, y) = (bx * 4 + px, by * 4 + py);
                    if x >= w || y >= h {
                        continue;
                    }
                    let i = ((bits >> ((py * 4 + px) * 2)) & 3) as usize;
                    let col = colors[i];
                    let a = if v0 <= v1 && i == 3 { 0 } else { 255 };
                    out[(y * w + x) * 4..][..4].copy_from_slice(&[col[0] as u8, col[1] as u8, col[2] as u8, a]);
                }
            }
        }
    }
    Some(out)
}

/// DXT5 (BC3) blocks -> RGBA8. Each 4x4 block: 8 bytes of interpolated alpha, 8 bytes of BC1 color.
fn dxt5_to_rgba(src: &[u8], w: u32, h: u32) -> Option<Vec<u8>> {
    let (bw, bh) = (w.div_ceil(4) as usize, h.div_ceil(4) as usize);
    if src.len() < bw * bh * 16 {
        return None;
    }
    let (w, h) = (w as usize, h as usize);
    let mut out = vec![0u8; w * h * 4];
    for by in 0..bh {
        for bx in 0..bw {
            let b = &src[(by * bw + bx) * 16..][..16];
            let (a0, a1) = (b[0] as u32, b[1] as u32);
            let mut alpha = [a0, a1, 0, 0, 0, 0, 0, 0];
            for i in 2..8 {
                alpha[i] = if a0 > a1 {
                    ((8 - i as u32) * a0 + (i as u32 - 1) * a1) / 7
                } else if i < 6 {
                    ((6 - i as u32) * a0 + (i as u32 - 1) * a1) / 5
                } else if i == 6 {
                    0
                } else {
                    255
                };
            }
            let abits = u64::from_le_bytes([b[2], b[3], b[4], b[5], b[6], b[7], 0, 0]);
            let c = |v: u16| [((v >> 11) * 255 / 31) as u32, (((v >> 5) & 63) * 255 / 63) as u32, ((v & 31) * 255 / 31) as u32];
            let (v0, v1) = (u16::from_le_bytes([b[8], b[9]]), u16::from_le_bytes([b[10], b[11]]));
            let (c0, c1) = (c(v0), c(v1));
            // DXT5 always uses the 4-color mode for its color block.
            let colors = [
                c0,
                c1,
                [0, 1, 2].map(|k| (2 * c0[k] + c1[k]) / 3),
                [0, 1, 2].map(|k| (c0[k] + 2 * c1[k]) / 3),
            ];
            let cbits = u32::from_le_bytes([b[12], b[13], b[14], b[15]]);
            for py in 0..4 {
                for px in 0..4 {
                    let (x, y) = (bx * 4 + px, by * 4 + py);
                    if x >= w || y >= h {
                        continue;
                    }
                    let i = py * 4 + px;
                    let col = colors[((cbits >> (i * 2)) & 3) as usize];
                    let a = alpha[((abits >> (i * 3)) & 7) as usize];
                    out[(y * w + x) * 4..][..4].copy_from_slice(&[col[0] as u8, col[1] as u8, col[2] as u8, a as u8]);
                }
            }
        }
    }
    Some(out)
}

impl Context3D for HostContext3D {
    fn profile(&self) -> Context3DProfile {
        self.profile
    }

    fn bitmap_handle(&self) -> BitmapHandle {
        self.front.clone().unwrap_or_else(|| BitmapHandle(Arc::new(HostTexture::new_empty(1, 1))))
    }

    fn back_buffer_handle(&self) -> Option<BitmapHandle> {
        self.back.clone()
    }

    fn back_buffer_size(&self) -> Option<(u32, u32)> {
        self.back_size
    }

    fn buffers_cleared(&self) -> bool {
        self.cleared
    }

    fn should_render(&self) -> bool {
        self.back.is_some()
    }

    fn disposed_index_buffer_handle(&self) -> Rc<dyn IndexBuffer> {
        Rc::new(C3dIndexBuffer(HostBuffer::new(4, false)))
    }

    fn disposed_vertex_buffer_handle(&self) -> Rc<dyn VertexBuffer> {
        Rc::new(C3dVertexBuffer { buf: HostBuffer::new(4, true), data32_per_vertex: 1 })
    }

    fn create_index_buffer(&mut self, _usage: BufferUsage, num_indices: u32) -> Box<dyn IndexBuffer> {
        Box::new(C3dIndexBuffer(HostBuffer::new(num_indices as usize * 2, false)))
    }

    fn create_vertex_buffer(&mut self, _usage: BufferUsage, num_vertices: u32, data_32_per_vertex: u8) -> Rc<dyn VertexBuffer> {
        Rc::new(C3dVertexBuffer {
            buf: HostBuffer::new(num_vertices as usize * data_32_per_vertex as usize * 4, true),
            data32_per_vertex: data_32_per_vertex,
        })
    }

    fn create_texture(
        &mut self,
        width: u32,
        height: u32,
        format: Context3DTextureFormat,
        _optimize_for_render_to_texture: bool,
        _streaming_levels: u32,
    ) -> Result<Rc<dyn Texture>, Error> {
        if matches!(format, Context3DTextureFormat::RgbaHalfFloat) {
            host::log(1, &format!("Stage3D: texture format {format:?} is not supported, using RGBA"));
        }
        let dxt5 = matches!(format, Context3DTextureFormat::CompressedAlpha);
        let dxt1 = matches!(format, Context3DTextureFormat::Compressed);
        Ok(Rc::new(C3dTexture { tex: Rc::new(HostTexture::new_empty(width, height)), cube: false, dxt5, dxt1 }))
    }

    fn create_cube_texture(
        &mut self,
        size: u32,
        format: Context3DTextureFormat,
        _optimize_for_render_to_texture: bool,
        _streaming_levels: u32,
    ) -> Result<Rc<dyn Texture>, Error> {
        // Faces +X, -X, +Y, -Y, +Z, -Z stacked top to bottom; CUBE_ATLAS_GLSL samples them.
        let dxt5 = matches!(format, Context3DTextureFormat::CompressedAlpha);
        let dxt1 = matches!(format, Context3DTextureFormat::Compressed);
        Ok(Rc::new(C3dTexture { tex: Rc::new(HostTexture::new_empty(size, size * 6)), cube: true, dxt5, dxt1 }))
    }

    fn upload_shaders(
        &mut self,
        module: &RefCell<Option<Rc<dyn ShaderModule>>>,
        vertex_shader_agal: Vec<u8>,
        fragment_shader_agal: Vec<u8>,
    ) -> Result<(), AgalError> {
        let vertex = naga_agal::parse_bytecode(&vertex_shader_agal)?;
        let fragment = naga_agal::parse_bytecode(&fragment_shader_agal)?;
        let fragment_samplers = naga_agal::extract_sampler_configs(&fragment)?;
        *module.borrow_mut() = Some(Rc::new(ShaderPair { vertex, fragment, fragment_samplers, programs: RefCell::new(HashMap::new()) }));
        Ok(())
    }

    fn process_command(&mut self, command: Context3DCommand<'_>) {
        match command {
            Context3DCommand::Clear { red, green, blue, alpha, depth, stencil, mask } => {
                self.cleared = true;
                if let Some(t) = self.target() {
                    let f = |v: f64| (v as f32).to_bits();
                    unsafe { host::host_c3d_clear(t.tex, f(red), f(green), f(blue), f(alpha), f(depth), stencil, mask) }
                }
            }
            Context3DCommand::ConfigureBackBuffer { width, height, depth_and_stencil, .. } => {
                let (width, height) = (width.max(1), height.max(1));
                self.front = Some(BitmapHandle(Arc::new(HostTexture::new_empty(width, height))));
                self.back = Some(BitmapHandle(Arc::new(HostTexture::new_empty(width, height))));
                self.back_size = Some((width, height));
                self.back_depth = depth_and_stencil;
                self.cleared = false;
            }
            Context3DCommand::SetRenderToTexture { texture, enable_depth_and_stencil, .. } => {
                // A texture can't be sampled while it is being drawn into.
                for t in &mut self.textures {
                    if t.as_ref().is_some_and(|b| Rc::ptr_eq(&b.texture, &texture)) {
                        *t = None;
                    }
                }
                self.rtt = Some((texture, enable_depth_and_stencil));
            }
            Context3DCommand::SetRenderToBackBuffer => self.rtt = None,
            Context3DCommand::UploadToIndexBuffer { buffer, start_offset, data } => {
                if let Some(ib) = <dyn std::any::Any>::downcast_mut::<C3dIndexBuffer>(buffer) {
                    ib.0.upload(start_offset * 2, data);
                }
            }
            Context3DCommand::UploadToVertexBuffer { buffer, start_vertex, data32_per_vertex, data } => {
                if let Ok(vb) = Rc::<dyn std::any::Any>::downcast::<C3dVertexBuffer>(buffer) {
                    vb.buf.upload(start_vertex * data32_per_vertex as usize * 4, data);
                }
            }
            Context3DCommand::DrawTriangles { index_buffer, first_index, num_triangles } => {
                self.draw(index_buffer, first_index, num_triangles);
            }
            Context3DCommand::SetVertexBufferAt { index, buffer, buffer_offset } => {
                let index = index as usize;
                if index < MAX_ATTRIBUTES {
                    self.attributes[index] = buffer.and_then(|(b, format)| {
                        let b = Rc::<dyn std::any::Any>::downcast::<C3dVertexBuffer>(b).ok()?;
                        Some(Attribute { buffer: b, offset_words: buffer_offset, format })
                    });
                }
            }
            Context3DCommand::SetShaders { module } => {
                self.shaders = module.and_then(|m| Rc::<dyn std::any::Any>::downcast::<ShaderPair>(m).ok());
                if let Some(s) = &self.shaders {
                    // A program's own sampler settings replace the current ones ('ignoresampler' keeps them).
                    for (i, c) in s.fragment_samplers.iter().enumerate() {
                        if let Some(c) = c {
                            self.samplers[i] = *c;
                        }
                    }
                }
            }
            Context3DCommand::SetProgramConstants { program_type, first_register, matrix_raw_data_column_major } => {
                let consts = match program_type {
                    ProgramType::Vertex => &mut self.vertex_constants,
                    ProgramType::Fragment => &mut self.fragment_constants,
                };
                let start = first_register as usize * 4;
                for (i, bytes) in matrix_raw_data_column_major.iter().enumerate() {
                    if let Some(slot) = consts.get_mut(start + i) {
                        *slot = f32::from_ne_bytes(*bytes);
                    }
                }
            }
            Context3DCommand::SetCulling { face: f } => self.culling = face(f),
            Context3DCommand::CopyBitmapToTexture { source, source_width, source_height, dest, layer, mip_level } => {
                if let Some(t) = <dyn std::any::Any>::downcast_ref::<C3dTexture>(dest.as_ref()) {
                    // (A cube texture's faces share one picture, which has no room for smaller versions.)
                    if mip_level > 0 && t.cube {
                        return;
                    }
                    let (w, h) = (source_width.min(t.width() >> mip_level), source_height.min(t.height() >> mip_level));
                    let y = if t.cube { layer.min(5) * t.width() } else { 0 };
                    // (Compressed textures may also get JPEG-XR ATF data, which comes as RGBA: dxt1_to_rgba checks the length.)
                    let rgba = if t.dxt5 {
                        dxt5_to_rgba(source, source_width, source_height)
                    } else if let Some(rgba) = t.dxt1.then(|| dxt1_to_rgba(source, source_width, source_height)).flatten() {
                        Some(rgba)
                    } else {
                        to_rgba(Context3DTextureFormat::Bgra, source, source_width, source_height)
                            .or_else(|| to_rgba(Context3DTextureFormat::BgraPacked, source, source_width, source_height))
                    };
                    if let Some(rgba) = rgba {
                        if mip_level > 0 {
                            let (w, h) = (w.max(1), h.max(1));
                            unsafe { host::host_tex_mip(t.tex.id, mip_level, w, h, rgba.as_ptr(), source_width * 4) }
                        } else {
                            unsafe { host::host_tex_update(t.tex.id, 0, y, w, h, rgba.as_ptr(), source_width * 4) }
                        }
                    }
                }
            }
            Context3DCommand::SetTextureAt { sampler, texture, cube: _ } => {
                let i = sampler as usize;
                if i < MAX_TEXTURES {
                    self.textures[i] = texture.map(|texture| BoundTexture { texture });
                }
            }
            Context3DCommand::SetColorMask { red, green, blue, alpha } => {
                self.color_mask = red as u32 | (green as u32) << 1 | (blue as u32) << 2 | (alpha as u32) << 3;
            }
            Context3DCommand::SetDepthTest { depth_mask, pass_compare_mode } => {
                self.depth_mask = depth_mask;
                self.depth_compare = compare(pass_compare_mode);
            }
            Context3DCommand::SetBlendFactors { source_factor, destination_factor } => {
                self.blend = (factor(source_factor), factor(destination_factor));
            }
            Context3DCommand::SetSamplerStateAt { sampler, wrap, filter, mip_filter } => {
                let i = sampler as usize;
                if i < MAX_TEXTURES {
                    self.samplers[i].wrapping = match wrap {
                        Context3DWrapMode::Clamp => naga_agal::Wrapping::Clamp,
                        Context3DWrapMode::Repeat => naga_agal::Wrapping::Repeat,
                        Context3DWrapMode::ClampURepeatV => naga_agal::Wrapping::ClampURepeatV,
                        Context3DWrapMode::RepeatUClampV => naga_agal::Wrapping::RepeatUClampV,
                    };
                    self.samplers[i].filter = match filter {
                        Context3DTextureFilter::Nearest => naga_agal::Filter::Nearest,
                        _ => naga_agal::Filter::Linear,
                    };
                    self.samplers[i].mipmap = match mip_filter {
                        1 => naga_agal::Mipmap::Nearest,
                        2 => naga_agal::Mipmap::Linear,
                        _ => naga_agal::Mipmap::Disable,
                    };
                }
            }
            Context3DCommand::SetScissorRectangle { rect } => {
                self.scissor = rect.map(|r| {
                    let x = r.x_min.to_pixels().max(0.0) as u32;
                    let y = r.y_min.to_pixels().max(0.0) as u32;
                    [x, y, r.width().to_pixels().max(0.0) as u32, r.height().to_pixels().max(0.0) as u32]
                });
            }
            Context3DCommand::SetStencilActions { triangle_face, compare_mode, on_both_pass, on_depth_fail, on_depth_pass_stencil_fail } => {
                self.stencil[0] = face(triangle_face);
                self.stencil[1] = compare(compare_mode);
                self.stencil[2] = stencil_op(on_both_pass);
                self.stencil[3] = stencil_op(on_depth_fail);
                self.stencil[4] = stencil_op(on_depth_pass_stencil_fail);
            }
            Context3DCommand::SetStencilReferenceValue { reference_value, read_mask, write_mask } => {
                self.stencil[5] = reference_value;
                self.stencil[6] = read_mask;
                self.stencil[7] = write_mask;
            }
        }
    }

    fn present(&mut self) {
        std::mem::swap(&mut self.front, &mut self.back);
        self.rtt = None;
        self.cleared = false;
    }
}
