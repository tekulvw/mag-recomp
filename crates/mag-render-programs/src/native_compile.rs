//! In-process HLSL legalization and portable target lowering. Compilation does
//! not acquire a device or submit work. Inputs and results are owned CPU data.
use crate::HlslProgram;
use naga::{AddressSpace, Binding, TypeInner};
use std::ffi::{CStr, CString, c_char};

#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub enum Stage {
    Vertex,
    Fragment,
}
impl Stage {
    fn naga(self) -> naga::ShaderStage {
        match self {
            Self::Vertex => naga::ShaderStage::Vertex,
            Self::Fragment => naga::ShaderStage::Fragment,
        }
    }
    fn group(self) -> u32 {
        if self == Self::Vertex { 0 } else { 1 }
    }
}
#[derive(Debug, PartialEq, Eq)]
pub struct BufferBinding {
    pub slot: u32,
    pub bytes: u32,
}
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub enum TextureDimension {
    D1,
    D2,
    D3,
    Cube,
}
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub enum TextureSampleKind {
    Float,
    Sint,
    Uint,
    Depth,
}
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub struct TextureBinding {
    pub slot: u32,
    pub dimension: TextureDimension,
    pub arrayed: bool,
    pub multisampled: bool,
    pub sample_kind: TextureSampleKind,
}
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub struct SamplerBinding {
    pub slot: u32,
    pub comparison: bool,
}
#[derive(Debug)]
pub struct NativeProgram {
    pub stage: Stage,
    pub msl: String,
    pub msl_entry: String,
    /// Native D3D HLSL, retaining source register classes and stage spaces.
    pub hlsl: String,
    pub hlsl_entry: String,
    pub spirv_entry: String,
    /// Canonical D3D clip coordinates; native Vulkan negative-height viewport
    /// supplies the single Y flip (same contract as the existing scene backend).
    pub spirv: Vec<u32>,
    pub buffers: Vec<BufferBinding>,
    pub textures: Vec<u32>,
    pub samplers: Vec<u32>,
    pub texture_bindings: Vec<TextureBinding>,
    pub sampler_bindings: Vec<SamplerBinding>,
    pub vertex_locations: Vec<u32>,
}
unsafe extern "C" {
    fn mrf_compile_spirv(
        source: *const c_char,
        stage: u32,
        words: *mut u32,
        capacity: u32,
        written: *mut u32,
        log: *mut c_char,
        log_capacity: u32,
    ) -> i32;
}
// Both stages use the same explicit varying locations even if one stage's
// optimizer drops unused declarations. Input attribute locations stay separate.
fn canonical_source(source: &str, stage: Stage) -> Result<String, String> {
    let marker = if stage == Stage::Vertex {
        "struct VSOutput {"
    } else {
        "struct PSInput {"
    };
    let start = source.find(marker).ok_or("wrong program stage")?;
    let end = start
        + source[start..]
            .find("};")
            .ok_or("missing varying structure end")?;
    let mut fields = source[start..end].to_owned();
    let mut mappings = vec![
        ("col0".to_owned(), 0),
        ("col1".to_owned(), 1),
        ("fog".to_owned(), 2),
    ];
    for index in 0..10 {
        mappings.push((
            format!(
                "{}{}",
                if stage == Stage::Vertex { "t" } else { "tc" },
                index
            ),
            index + 3,
        ));
    }
    for (name, location) in mappings {
        let declaration = format!("float4 {name}");
        // Include the colon/space to avoid t1 matching t10 if the emitter grows.
        for separator in [":", " :"] {
            let before = format!("{declaration}{separator}");
            let after = format!("[[vk::location({location})]] {before}");
            fields = fields.replace(&before, &after);
        }
    }
    for location in 0..16 {
        fields = fields.replace(
            &format!("centroid [[vk::location({location})]] float4"),
            &format!("[[vk::location({location})]] centroid float4"),
        );
    }
    let mut result = format!("{}{}{}", &source[..start], fields, &source[end..]);
    for class in ['b', 't', 's'] {
        for slot in 0..16 {
            result = result.replace(
                &format!("register({class}{slot})"),
                &format!("register({class}{slot}, space{})", stage.group()),
            );
        }
    }
    Ok(result)
}
fn remove_locations(source: &str) -> String {
    let mut text = source.to_owned();
    for location in 0..16 {
        text = text.replace(&format!("[[vk::location({location})]] "), "");
    }
    text
}
fn collect_locations(
    module: &naga::Module,
    ty: naga::Handle<naga::Type>,
    binding: &Option<Binding>,
    locations: &mut Vec<u32>,
) -> Result<(), String> {
    if let Some(Binding::Location { location, .. }) = binding {
        if *location >= 16 {
            return Err("vertex attribute location exceeds native layout".into());
        }
        locations.push(*location);
    } else if let TypeInner::Struct { members, .. } = &module.types[ty].inner {
        for member in members {
            collect_locations(module, member.ty, &member.binding, locations)?;
        }
    }
    Ok(())
}
// Naga30.0.1's SPIR-V importer applies default Perspective/Center together
// when interpolation is absent, replacing an explicit Centroid decoration.
// Preserve the actual SPIR-V sampling contract before validating/writing IR.
fn restore_centroid(words: &[u32], module: &mut naga::Module) -> Result<(), String> {
    if module.entry_points[0].stage != naga::ShaderStage::Fragment {
        return Ok(());
    }
    let mut locations = std::collections::BTreeMap::new();
    let mut centroid = std::collections::BTreeSet::new();
    let mut cursor = 5;
    while cursor < words.len() {
        let count = (words[cursor] >> 16) as usize;
        let instruction = words
            .get(cursor..cursor.checked_add(count).ok_or("SPIR-V extent")?)
            .ok_or("SPIR-V extent")?;
        if count == 0 {
            return Err("SPIR-V empty instruction".into());
        }
        if instruction[0] & 65535 == 71 && count >= 3 {
            // OpDecorate
            match instruction[2] {
                16 => {
                    centroid.insert(instruction[1]);
                } // Centroid
                30 if count == 4 => {
                    locations.insert(instruction[1], instruction[3]);
                } // Location
                _ => {}
            }
        }
        cursor += count;
    }
    for id in centroid {
        let location = locations
            .get(&id)
            .ok_or("centroid requires explicit location")?;
        let mut found = false;
        for argument in &mut module.entry_points[0].function.arguments {
            if let Some(Binding::Location {
                location: actual,
                sampling,
                ..
            }) = &mut argument.binding
                && actual == location
            {
                *sampling = Some(naga::Sampling::Centroid);
                found = true;
            }
        }
        if !found {
            return Err("unmapped centroid input".into());
        }
    }
    Ok(())
}
pub fn lower(program: &HlslProgram, stage: Stage) -> Result<NativeProgram, String> {
    if program.source.len() > 1024 * 1024 {
        return Err("HLSL exceeds owned compiler bound".into());
    }
    let source = canonical_source(&program.source, stage)?;
    let csource = CString::new(source.as_str()).map_err(|_| "HLSL contains NUL")?;
    let mut words = vec![0u32; 256 * 1024];
    let mut written = 0;
    let mut log = vec![0u8; 8192];
    // SAFETY: C bridge receives live, bounded, disjoint buffers; it catches C++
    // exceptions and serializes glslang's initialization/compiler access.
    let status = unsafe {
        mrf_compile_spirv(
            csource.as_ptr(),
            stage.group(),
            words.as_mut_ptr(),
            words.len() as u32,
            &mut written,
            log.as_mut_ptr().cast(),
            log.len() as u32,
        )
    };
    if status != 0 {
        return Err(format!(
            "glslang ({status}): {}",
            CStr::from_bytes_until_nul(&log)
                .map_err(|_| "unterminated compiler error")?
                .to_string_lossy()
        ));
    }
    if written == 0 || written as usize > words.len() {
        return Err("compiler returned invalid extent".into());
    }
    words.truncate(written as usize);
    let bytes: Vec<u8> = words.iter().flat_map(|word| word.to_le_bytes()).collect();
    // glslang consumes D3D clip coordinates without flipping Y. Keep that
    // canonical space on import and export. The native Vulkan scene uses a
    // negative-height viewport, so a writer Y flip here would double-flip.
    let options = naga::front::spv::Options {
        adjust_coordinate_space: false,
        ..Default::default()
    };
    let mut module = naga::front::spv::parse_u8_slice(&bytes, &options)
        .map_err(|e| format!("SPIR-V import: {e}"))?;
    if module.entry_points.len() != 1
        || module.entry_points[0].stage != stage.naga()
        || !module.overrides.is_empty()
    {
        return Err("unexpected entry points or specialization constants".into());
    }
    restore_centroid(&words, &mut module)?;
    let info = naga::valid::Validator::new(
        naga::valid::ValidationFlags::all(),
        naga::valid::Capabilities::empty(),
    )
    .validate(&module)
    .map_err(|e| format!("imported program validation: {e:?}"))?;
    let mut sizes = naga::proc::Layouter::default();
    sizes.update(module.to_ctx()).map_err(|e| e.to_string())?;
    let mut buffers = Vec::new();
    let mut textures = Vec::new();
    let mut samplers = Vec::new();
    let mut texture_bindings = Vec::new();
    let mut sampler_bindings = Vec::new();
    let mut resources = naga::back::msl::EntryPointResources::default();
    let mut spv = naga::back::spv::Options {
        lang_version: (1, 3),
        flags: naga::back::spv::WriterFlags::empty(),
        fake_missing_bindings: false,
        ..Default::default()
    };
    for (_, global) in module.global_variables.iter() {
        if global.space == AddressSpace::Private && global.binding.is_none() {
            continue;
        }
        let binding = global.binding.ok_or("unexpected unbound resource")?;
        if binding.group != stage.group() {
            return Err("resource in wrong stage namespace".into());
        }
        let mut target = naga::back::msl::BindTarget::default();
        match (&global.space, &module.types[global.ty].inner) {
            (AddressSpace::Uniform, _) if binding.binding < 16 => {
                if binding.binding != stage.group()
                    || sizes[global.ty].size != program.constant_buffer_bytes
                {
                    return Err("uniform layout differs from owned program contract".into());
                }
                target.buffer = Some(binding.binding as u8);
                buffers.push(BufferBinding {
                    slot: binding.binding,
                    bytes: sizes[global.ty].size,
                });
            }
            (
                AddressSpace::Handle,
                TypeInner::Image {
                    dim,
                    arrayed,
                    class,
                },
            ) if (16..32).contains(&binding.binding) => {
                let (sample_kind, multisampled) = match class {
                    naga::ImageClass::Sampled { kind, multi } => (
                        match kind {
                            naga::ScalarKind::Float => TextureSampleKind::Float,
                            naga::ScalarKind::Sint => TextureSampleKind::Sint,
                            naga::ScalarKind::Uint => TextureSampleKind::Uint,
                            _ => return Err("unsupported sampled scalar kind".into()),
                        },
                        *multi,
                    ),
                    naga::ImageClass::Depth { multi } => (TextureSampleKind::Depth, *multi),
                    _ => {
                        return Err(
                            "storage/external images are outside owned material contract".into(),
                        );
                    }
                };
                let slot = binding.binding - 16;
                target.texture = Some(slot as u8);
                textures.push(slot);
                texture_bindings.push(TextureBinding {
                    slot,
                    dimension: match dim {
                        naga::ImageDimension::D1 => TextureDimension::D1,
                        naga::ImageDimension::D2 => TextureDimension::D2,
                        naga::ImageDimension::D3 => TextureDimension::D3,
                        naga::ImageDimension::Cube => TextureDimension::Cube,
                    },
                    arrayed: *arrayed,
                    multisampled,
                    sample_kind,
                });
            }
            (AddressSpace::Handle, TypeInner::Sampler { comparison })
                if (32..48).contains(&binding.binding) =>
            {
                let slot = binding.binding - 32;
                target.sampler = Some(naga::back::msl::BindSamplerTarget::Resource(slot as u8));
                samplers.push(slot);
                sampler_bindings.push(SamplerBinding {
                    slot,
                    comparison: *comparison,
                });
            }
            _ => {
                return Err(format!(
                    "unsupported resource layout {:?} {:?}",
                    global.space, binding
                ));
            }
        }
        resources.resources.insert(binding, target);
        spv.binding_map.insert(
            binding,
            naga::back::spv::BindingInfo {
                descriptor_set: binding.group,
                binding: binding.binding,
                binding_array_size: None,
            },
        );
    }
    let entry = &module.entry_points[0];
    let mut msl_options = naga::back::msl::Options {
        lang_version: (2, 1),
        fake_missing_bindings: false,
        ..Default::default()
    };
    msl_options
        .per_entry_point_map
        .insert(entry.name.clone(), resources);
    let (msl, msl_info) =
        naga::back::msl::write_string(&module, &info, &msl_options, &Default::default())
            .map_err(|e| format!("MSL output: {e}"))?;
    let msl_entry = msl_info
        .entry_point_names
        .into_iter()
        .next()
        .ok_or("missing MSL entry")?
        .map_err(|e| format!("MSL bindings: {e:?}"))?;
    let spirv = naga::back::spv::write_vec(
        &module,
        &info,
        &spv,
        Some(&naga::back::spv::PipelineOptions {
            shader_stage: stage.naga(),
            entry_point: entry.name.clone(),
        }),
    )
    .map_err(|e| format!("Vulkan output: {e}"))?;
    let mut vertex_locations = Vec::new();
    if stage == Stage::Vertex {
        for arg in &entry.function.arguments {
            collect_locations(&module, arg.ty, &arg.binding, &mut vertex_locations)?;
        }
    }
    buffers.sort_by_key(|v| v.slot);
    textures.sort();
    samplers.sort();
    texture_bindings.sort_by_key(|b| b.slot);
    sampler_bindings.sort_by_key(|b| b.slot);
    vertex_locations.sort();
    Ok(NativeProgram {
        stage,
        msl,
        msl_entry,
        // D3D pixel SV_Position.w is reciprocal clip W too. The retained
        // emitter's old FXC comment is not the native frontend contract.
        hlsl: if stage == Stage::Fragment {
            format!(
                "#define RSX_FRAGCOORD_W_IS_RCP 1\n{}",
                remove_locations(&source)
            )
        } else {
            remove_locations(&source)
        },
        hlsl_entry: "main".into(),
        spirv_entry: entry.name.clone(),
        spirv,
        buffers,
        textures,
        samplers,
        texture_bindings,
        sampler_bindings,
        vertex_locations,
    })
}
