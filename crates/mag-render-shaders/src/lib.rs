//! Bounded portable shader translation. Translation is not GPU execution.

use naga::{
    AddressSpace, ArraySize, Handle, ScalarKind, ShaderStage, StorageAccess, Type, TypeInner,
};
use serde_json::{Value, json};

pub const NAGA_VERSION: &str = "30.0.1";
pub const METAL_SIZES_BUFFER: u8 = 30;

#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub enum Stage {
    Compute,
    Vertex,
    Fragment,
}
impl Stage {
    fn naga(self) -> ShaderStage {
        match self {
            Self::Compute => ShaderStage::Compute,
            Self::Vertex => ShaderStage::Vertex,
            Self::Fragment => ShaderStage::Fragment,
        }
    }
    fn label(self) -> &'static str {
        match self {
            Self::Compute => "compute",
            Self::Vertex => "vertex",
            Self::Fragment => "fragment",
        }
    }
    fn hlsl(self) -> &'static str {
        match self {
            Self::Compute => "cs_6_0",
            Self::Vertex => "vs_6_0",
            Self::Fragment => "ps_6_0",
        }
    }
}

fn float4(module: &naga::Module, ty: Handle<Type>) -> bool {
    matches!(
        module.types[ty].inner,
        TypeInner::Vector {
            size: naga::VectorSize::Quad,
            scalar: naga::Scalar {
                kind: ScalarKind::Float,
                width: 4
            }
        }
    )
}
fn location_zero(binding: &Option<naga::Binding>) -> bool {
    matches!(
        binding,
        Some(naga::Binding::Location {
            location: 0,
            blend_src: None,
            per_primitive: false,
            ..
        })
    )
}
fn float2(module: &naga::Module, ty: Handle<Type>) -> bool {
    matches!(
        module.types[ty].inner,
        TypeInner::Vector {
            size: naga::VectorSize::Bi,
            scalar: naga::Scalar {
                kind: ScalarKind::Float,
                width: 4
            }
        }
    )
}
fn position(binding: &Option<naga::Binding>) -> bool {
    matches!(
        binding,
        Some(naga::Binding::BuiltIn(naga::BuiltIn::Position { .. }))
    )
}
// Bounded scene layout: Float4 position input, optional Float2 UV varying,
// readonly fixed buffers and separately reflected sampled 2D images/samplers.
fn graphics_io(module: &naga::Module, stage: Stage) -> Result<(), String> {
    if stage == Stage::Compute {
        return Ok(());
    }
    let entry = &module.entry_points[0];
    let result = entry.function.result.as_ref();
    match stage {
        Stage::Vertex => {
            let result = result.ok_or("vertex entry requires position output")?;
            if entry.function.arguments.len() != 1 {
                return Err("vertex input requires exactly one location-zero float32x4".into());
            }
            let input = &entry.function.arguments[0];
            if !float4(module, input.ty) || !location_zero(&input.binding) {
                return Err("unsupported vertex input layout".into());
            }
            if float4(module, result.ty) && position(&result.binding) {
                return Ok(());
            }
            if let TypeInner::Struct { ref members, .. } = module.types[result.ty].inner
                && members.len() == 2
                && float4(module, members[0].ty)
                && position(&members[0].binding)
                && float2(module, members[1].ty)
                && location_zero(&members[1].binding)
            {
                return Ok(());
            }
            Err("vertex output requires position and optional location-zero float32x2 UV".into())
        }
        Stage::Fragment => {
            if result.is_some_and(|r| !float4(module, r.ty) || !location_zero(&r.binding)) {
                return Err("fragment output requires location-zero float32x4".into());
            }
            if entry.function.arguments.is_empty() {
                return Ok(());
            }
            if entry.function.arguments.len() == 1
                && float2(module, entry.function.arguments[0].ty)
                && location_zero(&entry.function.arguments[0].binding)
            {
                return Ok(());
            }
            Err("fragment input requires optional location-zero float32x2 UV".into())
        }
        Stage::Compute => unreachable!(),
    }
}

pub struct Compilation {
    pub manifest: Value,
    pub msl: String,
    pub hlsl: String,
    pub spirv: Vec<u32>,
}

fn layout(
    module: &naga::Module,
    sizes: &naga::proc::Layouter,
    ty: Handle<Type>,
) -> Result<Value, String> {
    let mut value = match &module.types[ty].inner {
        TypeInner::Scalar(scalar)
            if scalar.width == 4
                && matches!(
                    scalar.kind,
                    ScalarKind::Float | ScalarKind::Sint | ScalarKind::Uint
                ) =>
        {
            json!({"kind": "scalar", "scalar": format!("{:?}", scalar.kind), "width_bytes": 4})
        }
        TypeInner::Vector { size, scalar }
            if *size != naga::VectorSize::Tri
                && scalar.width == 4
                && matches!(
                    scalar.kind,
                    ScalarKind::Float | ScalarKind::Sint | ScalarKind::Uint
                ) =>
        {
            json!({"kind": "vector", "components": *size as u8,
                "scalar": format!("{:?}", scalar.kind), "width_bytes": 4})
        }
        TypeInner::Matrix {
            columns: naga::VectorSize::Quad,
            rows: naga::VectorSize::Quad,
            scalar,
        } if scalar.kind == ScalarKind::Float && scalar.width == 4 => {
            json!({"kind": "matrix", "columns": 4, "rows": 4, "column_stride_bytes": 16,
                "scalar": "Float", "width_bytes": 4, "major": "column"})
        }
        TypeInner::Array { base, size, stride } => {
            let length = match size {
                ArraySize::Constant(length) => Some(length.get()),
                ArraySize::Dynamic => None,
                ArraySize::Pending(_) => {
                    return Err("unsupported layout: override array size".into());
                }
            };
            json!({"kind": "array", "length": length, "stride_bytes": stride,
                "element": layout(module, sizes, *base)?})
        }
        TypeInner::Struct { members, .. } => {
            let members = members
                .iter()
                .map(|member| {
                    Ok(json!({
                        "name": member.name, "offset_bytes": member.offset,
                        "layout": layout(module, sizes, member.ty)?,
                    }))
                })
                .collect::<Result<Vec<Value>, String>>()?;
            json!({"kind": "struct", "members": members})
        }
        _ => {
            return Err(format!(
                "unsupported buffer layout: {:?}",
                module.types[ty].inner
            ));
        }
    };
    value["minimum_size_bytes"] = json!(sizes[ty].size);
    value["alignment_bytes"] = json!(sizes[ty].alignment * 1);
    Ok(value)
}

fn has_runtime_array(module: &naga::Module, ty: Handle<Type>) -> bool {
    match &module.types[ty].inner {
        TypeInner::Array {
            size: ArraySize::Dynamic,
            ..
        } => true,
        TypeInner::Struct { members, .. } => members
            .last()
            .is_some_and(|member| has_runtime_array(module, member.ty)),
        _ => false,
    }
}

/// Compile one compute entry point with explicit group-zero buffer bindings.
/// All writers use complete binding maps; missing mappings are errors.
pub fn compile(source: &str) -> Result<Compilation, String> {
    compile_stage(source, Stage::Compute)
}

/// Compile one explicitly selected stage. Graphics IO is deliberately limited
/// to the authored offscreen scene; compute bindings retain their prior API.
pub fn compile_stage(source: &str, stage: Stage) -> Result<Compilation, String> {
    let module =
        naga::front::wgsl::parse_str(source).map_err(|error| error.emit_to_string(source))?;
    let info = naga::valid::Validator::new(
        naga::valid::ValidationFlags::all(),
        naga::valid::Capabilities::empty(),
    )
    .validate(&module)
    .map_err(|error| error.emit_to_string(source))?;
    if module.entry_points.len() != 1 || module.entry_points[0].stage != stage.naga() {
        return Err(format!(
            "expected exactly one {} entry point",
            stage.label()
        ));
    }
    graphics_io(&module, stage)?;
    if !module.overrides.is_empty() {
        return Err("shader overrides require an explicit specialization contract".into());
    }
    let entry = &module.entry_points[0];
    if stage == Stage::Compute
        && (entry.workgroup_size.contains(&0)
            || entry
                .workgroup_size
                .iter()
                .try_fold(1u32, |size, value| size.checked_mul(*value))
                .is_none_or(|size| size > 256))
    {
        return Err("workgroup exceeds the portable 256-invocation contract".into());
    }
    let mut sizes = naga::proc::Layouter::default();
    sizes
        .update(module.to_ctx())
        .map_err(|error| error.to_string())?;
    let mut bindings = Vec::new();
    let mut images = Vec::new();
    let mut samplers = Vec::new();
    let mut metal_sizes = Vec::new();
    let mut msl_resources = naga::back::msl::EntryPointResources {
        sizes_buffer: Some(METAL_SIZES_BUFFER),
        ..Default::default()
    };
    let mut hlsl_options = naga::back::hlsl::Options {
        shader_model: naga::back::hlsl::ShaderModel::V6_0,
        fake_missing_bindings: false,
        ..Default::default()
    };
    let mut spv_options = naga::back::spv::Options {
        lang_version: (1, 3),
        flags: naga::back::spv::WriterFlags::empty(),
        fake_missing_bindings: false,
        ..Default::default()
    };
    for (handle, global) in module.global_variables.iter() {
        let binding = global
            .binding
            .as_ref()
            .ok_or("unsupported global without buffer binding")?;
        let group = if stage == Stage::Fragment { 1 } else { 0 };
        if binding.group != group || binding.binding >= 16 {
            return Err("bindings must use their stage group (compute/vertex=0, fragment=1) and slots 0..15".into());
        }
        if global.space == AddressSpace::Handle {
            if stage == Stage::Compute {
                return Err("compute image/sampler contract is unsupported".into());
            }
            let mut target = naga::back::msl::BindTarget::default();
            match module.types[global.ty].inner {
                TypeInner::Image {
                    dim: naga::ImageDimension::D2,
                    arrayed: false,
                    class,
                } => {
                    let kind = match class {
                        naga::ImageClass::Sampled {
                            kind: ScalarKind::Float,
                            multi: false,
                        } => "float",
                        naga::ImageClass::Depth { multi: false } => "depth",
                        _ => return Err(
                            "only non-multisampled float/depth sampled 2D textures are supported"
                                .into(),
                        ),
                    };
                    target.texture = Some(binding.binding as u8);
                    images.push(json!({"name":global.name,"group":group,"binding":binding.binding,"dimension":"2d","sample_kind":kind,"msl_texture_index":binding.binding,"hlsl_register_class":"t","hlsl_register":binding.binding,"hlsl_space":group,"spirv_descriptor_set":group,"spirv_binding":binding.binding}));
                }
                TypeInner::Sampler { comparison } => {
                    target.sampler = Some(naga::back::msl::BindSamplerTarget::Resource(
                        binding.binding as u8,
                    ));
                    // Naga's HLSL6 writer uses bounded sampler arrays and a per-group
                    // StructuredBuffer<uint> index indirection, not standalone sN.
                    hlsl_options.sampler_buffer_binding_map.insert(
                        naga::back::hlsl::SamplerIndexBufferKey { group },
                        naga::back::hlsl::BindTarget {
                            register: 31,
                            space: group as u8,
                            ..Default::default()
                        },
                    );
                    hlsl_options.sampler_heap_target.standard_samplers.space = 2;
                    hlsl_options.sampler_heap_target.comparison_samplers.space = 3;

                    samplers.push(json!({"name":global.name,"group":group,"binding":binding.binding,"comparison":comparison,"msl_sampler_index":binding.binding,"hlsl_sampler_index_entry":binding.binding,"hlsl_sampler_heap_space":if comparison{3}else{2},"spirv_descriptor_set":group,"spirv_binding":binding.binding}));
                }
                _ => return Err("unsupported graphics handle resource".into()),
            }
            msl_resources.resources.insert(*binding, target);
            hlsl_options.binding_map.insert(
                *binding,
                naga::back::hlsl::BindTarget {
                    register: binding.binding,
                    space: group as u8,
                    ..Default::default()
                },
            );
            spv_options.binding_map.insert(
                *binding,
                naga::back::spv::BindingInfo {
                    descriptor_set: group,
                    binding: binding.binding,
                    binding_array_size: None,
                },
            );
            continue;
        }
        if stage != Stage::Compute
            && (has_runtime_array(&module, global.ty)
                || matches!(global.space,AddressSpace::Storage{access} if access.contains(StorageAccess::STORE)))
        {
            return Err("graphics buffers require readonly fixed-size layouts".into());
        }
        let (space, access, register) = match global.space {
            AddressSpace::Uniform => ("uniform", "read", "b"),
            AddressSpace::Storage { access } if access.contains(StorageAccess::STORE) => {
                ("storage", "read_write", "u")
            }
            AddressSpace::Storage { .. } => ("storage", "read", "t"),
            _ => return Err("only uniform and storage buffers are supported".into()),
        };
        let buffer_layout = layout(&module, &sizes, global.ty)?;
        bindings.push(json!({
            "name": global.name, "group": binding.group, "binding": binding.binding,
            "address_space": space, "access": access, "layout": buffer_layout,
            "minimum_binding_bytes": sizes[global.ty].size,
            "msl_buffer_index": binding.binding,
            "hlsl_register_class": register, "hlsl_register": binding.binding, "hlsl_space": group,
            "spirv_descriptor_set": group, "spirv_binding": binding.binding,
        }));
        if has_runtime_array(&module, global.ty) {
            metal_sizes.push(json!({"binding": binding.binding,
                "offset_bytes": 4 * metal_sizes.len(), "generated_member": format!("size{}", handle.index())}));
        }
        msl_resources.resources.insert(
            *binding,
            naga::back::msl::BindTarget {
                buffer: Some(binding.binding as u8),
                mutable: access == "read_write",
                ..Default::default()
            },
        );
        hlsl_options.binding_map.insert(
            *binding,
            naga::back::hlsl::BindTarget {
                register: binding.binding,
                space: group as u8,
                ..Default::default()
            },
        );
        spv_options.binding_map.insert(
            *binding,
            naga::back::spv::BindingInfo {
                descriptor_set: group,
                binding: binding.binding,
                binding_array_size: None,
            },
        );
    }
    if stage == Stage::Compute && bindings.is_empty() {
        return Err("compute shader has no supported buffer bindings".into());
    }
    let mut msl_options = naga::back::msl::Options {
        lang_version: (2, 0),
        fake_missing_bindings: false,
        ..Default::default()
    };
    msl_options
        .per_entry_point_map
        .insert(entry.name.clone(), msl_resources);
    let (msl, msl_info) = naga::back::msl::write_string(
        &module,
        &info,
        &msl_options,
        &naga::back::msl::PipelineOptions::default(),
    )
    .map_err(|error| error.to_string())?;
    let msl_entry = msl_info
        .entry_point_names
        .into_iter()
        .next()
        .ok_or("missing MSL entry point")?
        .map_err(|error| format!("MSL entry point: {error:?}"))?;
    let mut hlsl = String::new();
    let hlsl_pipeline = naga::back::hlsl::PipelineOptions::default();
    let hlsl_info = naga::back::hlsl::Writer::new(&mut hlsl, &hlsl_options, &hlsl_pipeline)
        .write(&module, &info, None)
        .map_err(|error| error.to_string())?;
    let hlsl_entry = hlsl_info
        .entry_point_names
        .into_iter()
        .next()
        .ok_or("missing HLSL entry point")?
        .map_err(|error| format!("HLSL entry point: {error:?}"))?;
    let spirv = naga::back::spv::write_vec(
        &module,
        &info,
        &spv_options,
        Some(&naga::back::spv::PipelineOptions {
            shader_stage: stage.naga(),
            entry_point: entry.name.clone(),
        }),
    )
    .map_err(|error| error.to_string())?;
    let bytes: Vec<u8> = spirv.iter().flat_map(|word| word.to_le_bytes()).collect();
    let regenerated = naga::front::spv::parse_u8_slice(&bytes, &Default::default())
        .map_err(|error| format!("generated SPIR-V parse: {error}"))?;
    naga::valid::Validator::new(
        naga::valid::ValidationFlags::all(),
        naga::valid::Capabilities::empty(),
    )
    .validate(&regenerated)
    .map_err(|error| format!("generated SPIR-V validation: {error}"))?;
    let mut manifest = json!({
        "schema_version": 1, "naga_version": NAGA_VERSION, "translation_only": true,
        "validation": {"input_ir": "naga", "generated_spirv_ir": "naga_reparse",
            "metal_platform_compiler": "pending", "hlsl_platform_compiler": "pending",
            "spirv_tools": "pending", "gpu_execution": "pending"},
        "stage": stage.label(), "entry_point": entry.name, "workgroup_size": if stage == Stage::Compute { json!(entry.workgroup_size) } else { Value::Null },
        "targets": {"msl": {"version": "2.0", "entry_point": msl_entry},
            "hlsl": {"shader_model": stage.hlsl(), "entry_point": hlsl_entry},
            "spirv": {"version": "1.3", "vulkan_environment": "vulkan1.1", "entry_point": entry.name}},
        "buffers": bindings,
        "metal_sizes_buffer": if metal_sizes.is_empty() { Value::Null } else { json!({
            "buffer_index": METAL_SIZES_BUFFER, "size_bytes": 4 * metal_sizes.len(),
            "value_unit": "bound_buffer_bytes", "entries": metal_sizes,
        }) },
    });
    if stage == Stage::Vertex {
        manifest["vertex_stride_bytes"] = json!(16);
        manifest["vertex_inputs"] = json!([{"location":0,"format":"float32x4","offset_bytes":0,"size_bytes":16,
            "hlsl_semantic":"LOC","hlsl_semantic_index":0,"msl_attribute":0}]);
        manifest["position_output"] =
            json!({"builtin":"position","format":"float32x4","hlsl_semantic":"SV_Position"});
    } else if stage == Stage::Fragment {
        manifest["fragment_outputs"] = if entry.function.result.is_some() {
            json!([{"location":0,"format":"float32x4","hlsl_semantic":"SV_Target","hlsl_semantic_index":0}])
        } else {
            json!([])
        };
    }
    if !images.is_empty() {
        manifest["images"] = json!(images);
    }
    if !samplers.is_empty() {
        manifest["samplers"] = json!(samplers);
        manifest["hlsl_sampler_binding"] = json!({"index_buffer":{"register_class":"t","register":31,"space":if stage==Stage::Fragment{1}else{0},"stride_bytes":4,"entries":16},"standard_heap":{"register_class":"s","register":0,"space":2,"count":2048},"comparison_heap":{"register_class":"s","register":0,"space":3,"count":2048}});
    }
    let uv = match stage {
        Stage::Vertex => matches!(
            module.types[entry.function.result.as_ref().unwrap().ty].inner,
            TypeInner::Struct { .. }
        ),
        Stage::Fragment => !entry.function.arguments.is_empty(),
        Stage::Compute => false,
    };
    if uv {
        manifest["varyings"] = json!([{"location":0,"format":"float32x2","interpolation":"perspective","sampling":"center","hlsl_semantic":"LOC","hlsl_semantic_index":0}]);
    }
    Ok(Compilation {
        manifest,
        msl,
        hlsl,
        spirv,
    })
}
