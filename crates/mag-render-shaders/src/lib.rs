//! Deliberately bounded compute translation. Translation is not GPU execution.

use naga::{
    AddressSpace, ArraySize, Handle, ScalarKind, ShaderStage, StorageAccess, Type, TypeInner,
};
use serde_json::{Value, json};

pub const NAGA_VERSION: &str = "27.0.3";
pub const METAL_SIZES_BUFFER: u8 = 30;

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
    let module =
        naga::front::wgsl::parse_str(source).map_err(|error| error.emit_to_string(source))?;
    let info = naga::valid::Validator::new(
        naga::valid::ValidationFlags::all(),
        naga::valid::Capabilities::empty(),
    )
    .validate(&module)
    .map_err(|error| error.emit_to_string(source))?;
    if module.entry_points.len() != 1 || module.entry_points[0].stage != ShaderStage::Compute {
        return Err("expected exactly one compute entry point".into());
    }
    if !module.overrides.is_empty() {
        return Err("shader overrides require an explicit specialization contract".into());
    }
    let entry = &module.entry_points[0];
    if entry.workgroup_size.contains(&0)
        || entry
            .workgroup_size
            .iter()
            .try_fold(1u32, |size, value| size.checked_mul(*value))
            .is_none_or(|size| size > 256)
    {
        return Err("workgroup exceeds the portable 256-invocation contract".into());
    }
    let mut sizes = naga::proc::Layouter::default();
    sizes
        .update(module.to_ctx())
        .map_err(|error| error.to_string())?;
    let mut bindings = Vec::new();
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
        if binding.group != 0 || binding.binding >= 16 {
            return Err("buffer bindings must use group 0 and slots 0..15".into());
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
            "hlsl_register_class": register, "hlsl_register": binding.binding, "hlsl_space": 0,
            "spirv_descriptor_set": 0, "spirv_binding": binding.binding,
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
                ..Default::default()
            },
        );
        spv_options.binding_map.insert(
            *binding,
            naga::back::spv::BindingInfo {
                descriptor_set: 0,
                binding: binding.binding,
                binding_array_size: None,
            },
        );
    }
    if bindings.is_empty() {
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
            shader_stage: ShaderStage::Compute,
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
    let manifest = json!({
        "schema_version": 1, "naga_version": NAGA_VERSION, "translation_only": true,
        "validation": {"input_ir": "naga", "generated_spirv_ir": "naga_reparse",
            "metal_platform_compiler": "pending", "hlsl_platform_compiler": "pending",
            "spirv_tools": "pending", "gpu_execution": "pending"},
        "stage": "compute", "entry_point": entry.name, "workgroup_size": entry.workgroup_size,
        "targets": {"msl": {"version": "2.0", "entry_point": msl_entry},
            "hlsl": {"shader_model": "cs_6_0", "entry_point": hlsl_entry},
            "spirv": {"version": "1.3", "vulkan_environment": "vulkan1.1", "entry_point": entry.name}},
        "buffers": bindings,
        "metal_sizes_buffer": if metal_sizes.is_empty() { Value::Null } else { json!({
            "buffer_index": METAL_SIZES_BUFFER, "size_bytes": 4 * metal_sizes.len(),
            "value_unit": "bound_buffer_bytes", "entries": metal_sizes,
        }) },
    });
    Ok(Compilation {
        manifest,
        msl,
        hlsl,
        spirv,
    })
}
