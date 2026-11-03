#![cfg(feature = "native-compiler")]
use mag_render_programs::{HlslProgram, native_compile::*};
fn program(body: &str) -> HlslProgram {
    HlslProgram {
        source: format!("struct PSInput {{ float4 position : SV_Position; }};\n{body}"),
        instructions: 1,
        constant_slots: 0,
        input_mask: 0,
        current_attribute_mask: 0,
        constant_buffer_bytes: 0,
    }
}
#[test]
fn stored_depth_gate_uses_framebuffer_xy_and_distinct_hidden_resources() {
    for body in [
        "float4 main(PSInput input):SV_Target0 {return float4(1,0,0,1);}",
        "float4 main(PSInput input):SV_Target0 {return float4(input.position.xy,0,1);}",
    ] {
        let p = lower_metal_stored_depth(&program(body)).unwrap();
        assert!(p.msl.contains("[[texture(15)]]") && p.msl.contains("[[buffer(2)]]"));
        assert!(p.msl.contains("depth2d<float") && p.msl.contains(".read("));
        assert!(p.msl.contains("discard_fragment()"));
        assert_eq!(p.buffers, vec![BufferBinding { slot: 2, bytes: 16 }]);
        assert_eq!(p.texture_bindings[0].sample_kind, TextureSampleKind::Depth);
        assert!(p.samplers.is_empty());
        assert_eq!(
            p.stored_depth_aux,
            Some(StoredDepthAux {
                texture_slot: 15,
                bounds_buffer_slot: 2,
                bounds_bytes: 16
            })
        );
        assert!(p.hlsl.is_empty() && p.spirv.is_empty());
        if let Some(path) = std::env::var_os("MAG_STORED_DEPTH_MSL_TEST_OUTPUT") {
            std::fs::write(path, &p.msl).unwrap();
        }
    }
}
#[test]
fn refuses_depth_export_discard_and_hidden_binding_collision() {
    for body in [
        "float4 main(PSInput input):SV_Target0 {clip(input.position.x);return float4(1,0,0,1);}",
        "struct Out {float4 color:SV_Target0;float depth:SV_Depth;}; Out main(PSInput input) {Out o;o.color=1;o.depth=.5;return o;}",
        "Texture2D<float4> tex:register(t15);float4 main(PSInput input):SV_Target0 {return tex.Load(int3(input.position.xy,0));}",
    ] {
        assert!(lower_metal_stored_depth(&program(body)).is_err());
    }
}
