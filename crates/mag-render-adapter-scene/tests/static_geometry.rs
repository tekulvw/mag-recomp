use mag_render_adapter_scene::{
    cohort_geometry::static_geometry::*, geometry_bindings::VertexBinding,
};
const ONE: u32 = 0x3f800000;
const POS: [[u32; 4]; 1] = [[ONE, 0, 0, ONE]];
const NORMAL: [[u32; 4]; 1] = [[0, ONE, 0, 0]];
const TANGENT: [[u32; 4]; 1] = [[ONE, 0, 0, ONE ^ 0x80000000]];
fn input() -> StaticGeometryInput<'static> {
    const OUTPUT: [VertexBinding; 2] = [
        VertexBinding {
            declaration: 0,
            semantic: 2,
            slot: 2,
            format: 6,
            components: 1,
            stride: 24,
            byte_offset: 12,
            encoded_bytes: 4,
        },
        VertexBinding {
            declaration: 1,
            semantic: 3,
            slot: 9,
            format: 3,
            components: 4,
            stride: 24,
            byte_offset: 16,
            encoded_bytes: 8,
        },
    ];
    StaticGeometryInput {
        group: 1,
        mode: 8,
        workers: 2,
        optional_count: 0,
        palette_bytes: [0; 2],
        skinning_control: 16,
        positions: &POS,
        normals: Some(&NORMAL),
        tangents: Some(&TANGENT),
        output: &OUTPUT,
    }
}
#[test]
fn ordinary_source_banks_survive_without_deformation_or_output_repacking() {
    let p = prepare_static_geometry(input(), 48).unwrap();
    assert_eq!(p.positions, POS);
    assert_eq!(p.normals, NORMAL);
    assert_eq!(p.tangents.as_deref(), Some(TANGENT.as_slice()));
    assert!(p.weights.is_empty());
}
#[test]
fn dedicated_deformation_and_real_source_controls_cannot_be_treated_as_static() {
    for group in [0, 3, 4, 6] {
        let mut i = input();
        i.group = group;
        assert!(matches!(
            prepare_static_geometry(i, 48),
            Err(StaticGeometryError::DedicatedDeformation(_))
        ));
    }
    let mut i = input();
    i.optional_count = 1;
    assert!(matches!(
        prepare_static_geometry(i, 48),
        Err(StaticGeometryError::Optional)
    ));
    let mut i = input();
    i.palette_bytes = [0, 48];
    assert!(matches!(
        prepare_static_geometry(i, 48),
        Err(StaticGeometryError::Palette)
    ));
    let mut i = input();
    i.skinning_control = 17;
    assert!(matches!(
        prepare_static_geometry(i, 48),
        Err(StaticGeometryError::DirectionTransform)
    ));
    let mut i = input();
    i.mode = 0x28;
    assert!(matches!(
        prepare_static_geometry(i, 48),
        Err(StaticGeometryError::Mode)
    ));
    let mut i = input();
    i.normals = None;
    assert!(matches!(
        prepare_static_geometry(i, 48),
        Err(StaticGeometryError::MissingSemantic(2))
    ));
    assert!(matches!(
        prepare_static_geometry(input(), 47),
        Err(StaticGeometryError::Budget)
    ));
}

#[test]
fn output_fetch_domain_is_distinct_from_raw_bank_precision() {
    let mut i = input();
    let normal = [[0, ONE, 0, 0x7fc00000]];
    i.normals = Some(&normal);
    // CMP fetch has XYZ plus defaultW, so unused bankW is not an input.
    assert!(prepare_static_geometry(i, 48).is_ok());
    let mut output = input().output.to_vec();
    output[0].format = 4;
    output[0].components = 3;
    let mut i = input();
    i.output = &output;
    assert!(matches!(
        prepare_static_geometry(i, 48),
        Err(StaticGeometryError::OutputFormat {
            semantic: 2,
            format: 4
        })
    ));
    let normal = [[0, 0x40000000, 0, 0]];
    let mut i = input();
    i.normals = Some(&normal);
    assert!(matches!(
        prepare_static_geometry(i, 48),
        Err(StaticGeometryError::OutputRange {
            semantic: 2,
            format: 6
        })
    ));
    output[0].format = 2;
    let mut i = input();
    i.normals = Some(&normal);
    i.output = &output;
    assert_eq!(prepare_static_geometry(i, 48).unwrap().normals, normal);
}
