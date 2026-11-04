#![cfg(feature = "owned-scene")]
use mag_render_adapter_core::scene_programs::*;
use mag_render_programs::{native_compile::*, *};
use std::sync::Arc;

#[test]
fn actual_compiled_vertex_owns_three_targets_and_native_uniform_contract() {
    let src = 0x1b02;
    let decoded = vertex(VertexProgram {
        words: &[
            (1 << 30) | (63 << 15),
            (1 << 22) | (src >> 9),
            (src & 511) << 23,
            (15 << 13) | 1,
        ],
        start_slot: 0,
        vertex_textures: 0,
        enabled_arrays: 1,
        known_current_attributes: 0,
    })
    .unwrap();
    let native = Arc::new(lower(&decoded, Stage::Vertex).unwrap());
    let packet = ProgramPacket::new(native.clone()).unwrap();
    for (backend, language, len, ptr) in [
        (
            Backend::Metal,
            1,
            native.msl.len(),
            native.msl.as_ptr().cast(),
        ),
        (
            Backend::D3d12,
            2,
            native.hlsl.len(),
            native.hlsl.as_ptr().cast(),
        ),
        (
            Backend::Vulkan,
            3,
            native.spirv.len() * 4,
            native.spirv.as_ptr().cast(),
        ),
    ] {
        let view = packet.view(backend).unwrap();
        assert_eq!(view.stage, 0);
        assert_eq!(view.shader.language, language);
        assert_eq!(view.shader.source_length, len as u64);
        assert_eq!(view.shader.source, ptr);
        assert_eq!(view.vertex_location_count, 1);
        assert_eq!(view.buffer_count, 1);
    }
    let b = &packet.uniform_layouts()[0];
    assert_eq!(
        (b.slot, b.access, b.binding_kind, b.minimum_bytes),
        (0, 1, 1, 8224)
    );
    assert_eq!(
        (
            b.runtime_size_index,
            b.runtime_array_stride,
            b.alignment_bytes
        ),
        (u32::MAX, 0, 16)
    );
    drop(native);
    assert!(packet.view(Backend::Vulkan).is_ok());
}

#[test]
fn actual_fragment_keeps_typed_sampler_and_texture_reflection() {
    let words: [u32; 4] = [
        (0x17 << 24) | (15 << 9) | (4 << 13) | 1,
        1 | 0x1c800 | (7 << 18),
        0x1c800,
        0x1c800,
    ];
    let bytes: Vec<_> = words
        .iter()
        .flat_map(|w| w.rotate_left(16).to_be_bytes())
        .collect();
    let decoded = fragment(FragmentProgram {
        bytes: &bytes,
        shader_control: 0x40,
        cube_mask: 0,
        volume_mask: 0,
        shadow: [ShadowTexture::default(); 16],
        gamma: [0; 16],
        expand: [0; 16],
        alpha_function: Some(0x204),
        srgb_output: false,
        sample_grid: [1, 1],
        unnormalized_mask: 0,
        texture_dimensions: [[1, 1]; 16],
    })
    .unwrap();
    let native = lower(&decoded, Stage::Fragment).unwrap();
    let packet = ProgramPacket::new(Arc::new(native)).unwrap();
    let view = packet.view(Backend::D3d12).unwrap();
    assert_eq!(
        (
            view.stage,
            view.texture_count,
            view.sampler_count,
            view.vertex_location_count
        ),
        (1, 1, 1, 0)
    );
    let t = &packet.texture_layouts()[0];
    assert_eq!(
        (
            t.slot,
            t.dimension,
            t.sample_kind,
            t.arrayed,
            t.multisampled
        ),
        (0, 2, 0, 0, 0)
    );
    assert_eq!(
        (
            packet.sampler_layouts()[0].slot,
            packet.sampler_layouts()[0].comparison
        ),
        (0, 0)
    );
    assert_eq!(packet.uniform_layouts()[0].slot, 1);
}

#[test]
fn typed_stored_depth_aux_is_internal_and_not_a_user_binding() {
    let source=HlslProgram {source:"struct PSInput {float4 position:SV_Position;}; float4 main(PSInput i):SV_Target0{return float4(1,0,0,1);}".into(),instructions:1,constant_slots:0,input_mask:0,current_attribute_mask:0,constant_buffer_bytes:0};
    let native = lower_metal_stored_depth(&source).unwrap();
    let packet = ProgramPacket::new(Arc::new(native)).unwrap();
    let view = packet.view(Backend::Metal).unwrap();
    assert_eq!(
        view.reserved,
        mag_render_abi::packet::PROGRAM_STORED_DEPTH_AUX
    );
    assert_eq!(
        (view.buffer_count, view.texture_count, view.sampler_count),
        (0, 0, 0)
    );
    assert!(packet.view(Backend::Vulkan).is_err());
    assert!(packet.view(Backend::D3d12).is_err());
    let mut bad = lower_metal_stored_depth(&source).unwrap();
    bad.buffers[0].bytes = 32;
    assert!(ProgramPacket::new(Arc::new(bad)).is_err());
    let mut bad = lower_metal_stored_depth(&source).unwrap();
    bad.texture_bindings[0].sample_kind = TextureSampleKind::Float;
    assert!(ProgramPacket::new(Arc::new(bad)).is_err());
}
