//! Pixel Bender shaders: the translator gives a naga module, which becomes GLSL and is compiled on the console
//! like Stage3D's shaders. A shader draws a full-target quad with its inputs and parameters.

use crate::host;
use crate::render::{Texture, as_texture};
use naga::back::glsl;
use ruffle_render::backend::RawTexture as _;
use ruffle_render::pixel_bender::{
    OUT_COORD_NAME, PixelBenderParam, PixelBenderShader, PixelBenderShaderHandle,
    PixelBenderShaderImpl, PixelBenderType,
};
use ruffle_render::pixel_bender_support::{ImageInputTexture, PixelBenderShaderArgument};
use std::rc::Rc;

const VERTEX_GLSL: &str = "#version 420 core
layout(location = 0) in vec2 _p2vs_location0;
layout(location = 0) smooth out vec2 _vs2fs_location0;
void main() {
    gl_Position = vec4(_p2vs_location0.x * 2.0 - 1.0, 1.0 - _p2vs_location0.y * 2.0, 0.0, 1.0);
    _vs2fs_location0 = _p2vs_location0;
}
";

#[derive(Debug)]
pub struct HostShader {
    shader: PixelBenderShader,
    program: u32,
    /// Inputs read with linear filtering rather than nearest, by input number.
    linear_mask: u32,
}

impl PixelBenderShaderImpl for HostShader {
    fn parsed_shader(&self) -> &PixelBenderShader {
        &self.shader
    }
}

impl Drop for HostShader {
    fn drop(&mut self) {
        if self.program != 0 {
            unsafe { host::host_c3d_program_free(self.program) }
        }
    }
}

fn to_glsl(module: &naga::Module) -> Result<(String, u32), String> {
    let info = naga::valid::Validator::new(naga::valid::ValidationFlags::all(), naga::valid::Capabilities::all())
        .validate(module)
        .map_err(|e| format!("{e:?}"))?;
    let mut binding_map = glsl::BindingMap::default();
    for b in naga_pixelbender::SHADER_FLOAT_PARAMETERS_INDEX..=naga_pixelbender::ZEROED_OUT_OF_RANGE_MODE_INDEX {
        binding_map.insert(naga::ResourceBinding { group: 0, binding: b }, 0);
    }
    for i in 0..8u32 {
        let binding = naga_pixelbender::TEXTURE_START_BIND_INDEX + i;
        binding_map.insert(naga::ResourceBinding { group: 0, binding }, i as u8);
    }
    for i in 0..3u32 {
        binding_map.insert(naga::ResourceBinding { group: 0, binding: i }, 0);
    }
    let options = glsl::Options {
        version: glsl::Version::Desktop(420),
        writer_flags: glsl::WriterFlags::empty(),
        binding_map,
        zero_initialize_workgroup_memory: false,
    };
    let pipeline = glsl::PipelineOptions {
        shader_stage: naga::ShaderStage::Fragment,
        entry_point: naga_pixelbender::FRAGMENT_SHADER_ENTRYPOINT.to_string(),
        multiview: None,
    };
    let mut out = String::new();
    let mut writer = glsl::Writer::new(&mut out, module, &info, &options, &pipeline, naga::proc::BoundsCheckPolicies::default())
        .map_err(|e| format!("{e:?}"))?;
    let reflection = writer.write().map_err(|e| format!("{e:?}"))?;
    // GLSL has one sampler per texture: which filter each input uses is set when it's bound.
    let mut linear_mask = 0;
    for mapping in reflection.texture_mapping.values() {
        let image = &module.global_variables[mapping.texture];
        let Some(sampler) = mapping.sampler else { continue };
        let sampler = &module.global_variables[sampler];
        if let (Some(i), Some(s)) = (&image.binding, &sampler.binding)
            && s.binding != naga_pixelbender::SAMPLER_CLAMP_NEAREST
        {
            linear_mask |= 1 << (i.binding - naga_pixelbender::TEXTURE_START_BIND_INDEX);
        }
    }
    Ok((flatten_shader(&crate::context3d::blocks_to_plain_uniforms(&out)), linear_mask))
}

/// Writes `cross(a, b)` out as arithmetic: with it, BlurredFocus came out green even where both inputs were zero.
fn expand_cross(line: &str) -> String {
    let Some((before, rest)) = line.split_once("cross(") else { return line.to_string() };
    let Some((args, after)) = rest.split_once(')') else { return line.to_string() };
    let Some((a, b)) = args.split_once(", ") else { return line.to_string() };
    format!(
        "{before}vec3({a}.y * {b}.z - {a}.z * {b}.y, {a}.z * {b}.x - {a}.x * {b}.z, {a}.x * {b}.y - {a}.y * {b}.x){after}"
    )
}

/// Makes the shader straight-line code. The compiler hung in Cemu on `if` blocks and on functions taking a sampler
/// (the same source compiles in 50 ms on a PC), and a shader has no side effects, so both branches can always be
/// worked out: each assignment in a branch becomes `x = condition ? value : x`.
fn flatten_shader(src: &str) -> String {
    const TYPES: [&str; 13] =
        ["vec2", "vec3", "vec4", "ivec2", "ivec3", "ivec4", "bvec2", "bvec3", "bvec4", "float", "int", "bool", "uint"];
    let mut out = String::with_capacity(src.len() * 2);
    let mut conds: Vec<String> = Vec::new();
    let mut next = 0;
    let mut lines = src.lines();
    while let Some(line) = lines.next() {
        if line.starts_with("vec4 sampler_wrapper(") {
            for inner in lines.by_ref() {
                if inner == "}" {
                    break;
                }
            }
            continue;
        }
        let line = expand_cross(&line.replace("sampler_wrapper(", "texture("));
        let t = line.trim();
        if let Some(rest) = t.strip_prefix("if ") {
            let cond = rest.trim_end_matches('{').trim();
            let name = format!("flat_cond_{next}");
            next += 1;
            let parent = conds.last().map_or("true".to_string(), |c| c.clone());
            out.push_str(&format!("bool {name} = ({parent}) && {cond};\nbool {name}_else = ({parent}) && !({name});\n"));
            conds.push(name);
            continue;
        }
        if t == "} else {" {
            if let Some(c) = conds.last_mut() {
                c.push_str("_else");
            }
            continue;
        }
        if t == "}" && !conds.is_empty() {
            conds.pop();
            continue;
        }
        if let Some(cond) = conds.last() {
            let first = t.split(' ').next().unwrap_or("");
            if !TYPES.contains(&first)
                && let Some((lhs, rhs)) = t.split_once(" = ")
            {
                let rhs = rhs.trim_end_matches(';');
                out.push_str(&format!("{lhs} = {cond} ? ({rhs}) : {lhs};\n"));
                continue;
            }
        }
        out.push_str(&line);
        out.push('\n');
    }
    propagate_immediates(&merge_component_writes(&out))
}

/// naga parks a shader's literal numbers in register 0's z and w (`local_float_reg_0_.z = float(vec4(0.5).x);`)
/// and reads them back through a copy (`vec4 _e9 = local_float_reg_0_; ... * _e9.zzzz`). CafeGLSL got that wrong:
/// a blend shader's `(a + b) * 0.5` kept a stale x lane (shader_as_mask), and writing the literal in place of
/// the read fixed it. So every such read becomes the literal(s) again.
fn propagate_immediates(src: &str) -> String {
    const REG0: &str = "local_float_reg_0_";
    let mut consts: [Option<String>; 4] = [None, None, None, None];
    let mut aliases: Vec<(String, [Option<String>; 4])> = Vec::new();
    let mut out = String::with_capacity(src.len());
    for line in src.lines() {
        let t = line.trim();
        let mut line = line.to_string();
        if !aliases.is_empty() {
            line = replace_alias_swizzles(&line, &aliases);
        }
        if let Some(rest) = t.strip_prefix("vec4 ")
            && let Some((name, value)) = rest.split_once(" = ")
            && value == format!("{REG0};")
            && name.starts_with("_e")
        {
            aliases.push((name.to_string(), consts.clone()));
        } else if let Some(rest) = t.strip_prefix(REG0) {
            if let Some(rest) = rest.strip_prefix('.')
                && let Some(c) = rest.chars().next()
                && let Some(lane) = "xyzw".find(c)
            {
                let value = rest[1..].trim_start_matches(" = ").trim_end_matches(';');
                consts[lane] = value
                    .strip_prefix("float(vec4(")
                    .and_then(|v| v.strip_suffix(").x)"))
                    .filter(|v| v.parse::<f64>().is_ok())
                    .map(|v| v.to_string());
            } else if rest.starts_with(" = ") {
                consts = [None, None, None, None];
            }
        }
        out.push_str(&line);
        out.push('\n');
    }
    out
}

/// `_eN.zw`-style reads of an alias of register 0 whose lanes are all known literals become those literals.
fn replace_alias_swizzles(line: &str, aliases: &[(String, [Option<String>; 4])]) -> String {
    let mut out = String::with_capacity(line.len());
    let mut rest = line;
    while let Some(pos) = rest.find("_e") {
        let (before, from) = rest.split_at(pos);
        out.push_str(before);
        let digits = from[2..].chars().take_while(|c| c.is_ascii_digit()).count();
        let name = &from[..2 + digits];
        let after = &from[2 + digits..];
        let swz: String = after.strip_prefix('.').map_or(String::new(), |s| {
            s.chars().take_while(|c| "xyzw".contains(*c)).take(4).collect()
        });
        let next_ok = after.get(1 + swz.len()..).and_then(|s| s.chars().next()).is_none_or(|c| !c.is_ascii_alphanumeric() && c != '_');
        let known = (digits > 0 && !swz.is_empty() && next_ok)
            .then(|| aliases.iter().rev().find(|(n, _)| n == name))
            .flatten()
            .and_then(|(_, consts)| {
                swz.chars().map(|c| consts["xyzw".find(c).unwrap()].clone()).collect::<Option<Vec<String>>>()
            });
        match known {
            Some(values) => {
                if values.len() == 1 {
                    out.push_str(&values[0]);
                } else {
                    out.push_str(&format!("vec{}({})", values.len(), values.join(", ")));
                }
                rest = &after[1 + swz.len()..];
            }
            None => {
                out.push_str(name);
                rest = after;
            }
        }
    }
    out.push_str(rest);
    out
}

/// naga copies a register one component at a time (`r.x = float(e.x); r.y = float(e.y); ...`), and CafeGLSL dropped
/// one of those writes now and then: a blend shader's `a + b` came out as `(b.x + b.x, a.y + b.y, ...)`. Four such
/// lines from the same source become one vector assignment.
fn merge_component_writes(src: &str) -> String {
    let lines: Vec<&str> = src.lines().collect();
    let mut out = String::with_capacity(src.len());
    let mut i = 0;
    while i < lines.len() {
        if i + 3 < lines.len()
            && let Some((name, source)) = component_write(lines[i], 'x')
            && component_write(lines[i + 1], 'y') == Some((name, source))
            && component_write(lines[i + 2], 'z') == Some((name, source))
            && component_write(lines[i + 3], 'w') == Some((name, source))
        {
            out.push_str(&format!("    {name} = vec4({source});\n"));
            i += 4;
            continue;
        }
        out.push_str(lines[i]);
        out.push('\n');
        i += 1;
    }
    out
}

/// `name.c = float(source.c);` -> (name, source), for the component `c`.
fn component_write(line: &str, c: char) -> Option<(&str, &str)> {
    let t = line.trim();
    let (name, rest) = t.split_once(&format!(".{c} = float(")).filter(|(n, _)| !n.contains(' '))?;
    let source = rest.strip_suffix(&format!(".{c});"))?;
    (!source.contains(' ') && !source.is_empty()).then_some((name, source))
}
pub fn compile(shader: PixelBenderShader) -> PixelBenderShaderHandle {
    let built = naga_pixelbender::ShaderBuilder::build(&shader);
    let (program, linear_mask) = match built.map_err(|e| format!("{e:?}")).and_then(|m| to_glsl(&m.fragment)) {
        Ok((fs, linear_mask)) => {
            // For looking at what CafeGLSL gets (the last shader compiled); overwritten each time. A pb_override.glsl
            // next to it is compiled instead, for trying variants without a rebuild.
            let _ = std::fs::write("/wiiu/apps/flashplayeremu/pb_last.glsl", &fs);
            let fs = std::fs::read_to_string("/wiiu/apps/flashplayeremu/pb_override.glsl").unwrap_or(fs);
            (
                unsafe { host::host_c3d_program(VERTEX_GLSL.as_ptr(), VERTEX_GLSL.len() as u32, fs.as_ptr(), fs.len() as u32) },
                linear_mask,
            )
        }
        Err(e) => {
            host::log(0, &format!("Pixel Bender: couldn't translate {:?}: {e}", shader.name));
            (0, 0)
        }
    };
    PixelBenderShaderHandle(std::sync::Arc::new(HostShader { shader, program, linear_mask }))
}

fn as_host_shader(handle: &PixelBenderShaderHandle) -> &HostShader {
    <dyn std::any::Any>::downcast_ref(&*handle.0).expect("Pixel Bender shader from another backend")
}

fn push_value(value: &PixelBenderType, floats: &mut Vec<f32>, ints: &mut Vec<i32>) {
    use PixelBenderType::*;
    match value {
        &TFloat(a) => floats.extend([a, 0.0, 0.0, 0.0]),
        &TFloat2(a, b) => floats.extend([a, b, 0.0, 0.0]),
        &TFloat3(a, b, c) => floats.extend([a, b, c, 0.0]),
        &TFloat4(a, b, c, d) => floats.extend([a, b, c, d]),
        &TInt(a) | &TBool(a) => ints.extend([a as i32, 0, 0, 0]),
        &TInt2(a, b) | &TBool2(a, b) => ints.extend([a as i32, b as i32, 0, 0]),
        &TInt3(a, b, c) | &TBool3(a, b, c) => ints.extend([a as i32, b as i32, c as i32, 0]),
        &TInt4(a, b, c, d) | &TBool4(a, b, c, d) => ints.extend([a as i32, b as i32, c as i32, d as i32]),
        // Matrices by column, each a vec4.
        TFloat2x2(m) => floats.extend(m),
        TFloat3x3(m) => {
            for c in m.chunks_exact(3) {
                floats.extend([c[0], c[1], c[2], 0.0]);
            }
        }
        TFloat4x4(m) => floats.extend(m),
        _ => {}
    }
}

/// Runs a shader into a target (0: the screen). `zero_outside`: samples outside an input read as transparent
/// (filters and blends), otherwise as its edge (ShaderJob).
pub fn run(
    handle: &PixelBenderShaderHandle,
    arguments: &[PixelBenderShaderArgument],
    target: u32,
    zero_outside: bool,
) -> bool {
    let shader = as_host_shader(handle);
    if shader.program == 0 {
        return false;
    }
    let mut textures = [0u32; 8];
    let mut keep: Vec<Rc<Texture>> = Vec::new();
    let (mut floats, mut ints) = (Vec::new(), Vec::new());
    for arg in arguments {
        match arg {
            PixelBenderShaderArgument::ImageInput { index, texture, .. } => {
                let slot = *index as usize;
                if slot >= textures.len() {
                    continue;
                }
                textures[slot] = match texture {
                    Some(ImageInputTexture::Bitmap(handle)) => {
                        let id = as_texture(handle).id;
                        if id == target {
                            // The target can't be read while being drawn into.
                            let t = as_texture(handle);
                            let copy = Rc::new(Texture::new_empty(t.width, t.height));
                            unsafe { host::host_tex_copy(copy.id, id) };
                            let copy_id = copy.id;
                            keep.push(copy);
                            copy_id
                        } else {
                            id
                        }
                    }
                    Some(ImageInputTexture::Floats { width, height, data }) => {
                        let channels = data.channel_count() as usize;
                        let raw = data.padded_data();
                        let per_pixel = if channels == 3 { 4 } else { channels };
                        let mut rgba = vec![0f32; (*width * *height * 4) as usize];
                        for (out, src) in rgba.chunks_exact_mut(4).zip(raw.chunks_exact(per_pixel)) {
                            out[..per_pixel].copy_from_slice(src);
                        }
                        let id = unsafe { host::host_tex_create_float(*width, *height, rgba.as_ptr()) };
                        keep.push(Rc::new(Texture { id, width: *width, height: *height }));
                        id
                    }
                    _ => 0,
                };
            }
            PixelBenderShaderArgument::ValueInput { index, value } => {
                let is_out_coord = matches!(
                    shader.shader.params.get(*index as usize),
                    Some(PixelBenderParam::Normal { name, .. }) if name == OUT_COORD_NAME
                );
                if !is_out_coord {
                    push_value(value, &mut floats, &mut ints);
                }
            }
        }
    }
    let done = unsafe {
        host::host_pb_run(
            shader.program,
            target,
            textures.as_ptr(),
            floats.as_ptr(),
            floats.len() as u32,
            ints.as_ptr(),
            ints.len() as u32,
            zero_outside as u32,
            shader.linear_mask,
        )
    };
    done != 0
}
