//! The cohort capture's deformation call sites (groups 3/4, palette skinning
//! and group 0). Each adapter entry point must return exactly what the shared
//! glue entry point returns for the same guest words, so Main and the depth
//! pre-pass can never disagree, and must keep the capture's own budget and
//! population refusals before any arithmetic.
use mag_render_adapter_scene::deformation::*;
use mag_render_behavior::{group_zero, noninstanced, skinning};
use mag_render_behavior_glue::deformation::{self as shared, Prepared};

fn bits(v: [f32; 4]) -> [u32; 4] {
    v.map(f32::to_bits)
}
fn bank(n: usize, seed: u32, w: f32) -> Vec<[u32; 4]> {
    let mut x = seed.wrapping_mul(2_654_435_761) | 1;
    let mut r = move || {
        x ^= x << 13;
        x ^= x >> 17;
        x ^= x << 5;
        (x % 2000) as f32 / 1000.0 - 1.0
    };
    (0..n).map(|_| bits([r(), r(), r(), w])).collect()
}

#[test]
fn noninstanced_site_returns_the_shared_banks_and_keeps_its_refusals() {
    for group in [3u8, 4] {
        let positions = bank(37, 1, 1.0);
        let normals = bank(37, 2, 0.0);
        let weights = bank(37, 3, 0.0);
        let tangents = bank(37, 4, 1.0);
        let tangents = (group == 4).then_some(tangents.as_slice());
        let material_rows = [
            bits([0.3, 0.2, 0.5, 0.1]),
            bits([1.5, 0.7, 2.0, 0.0]),
            bits([0.9, 0.4, 3.0, 0.0]),
        ];
        let wind = [1.1f32, 0.0, 0.4, 0.6, 123.25].map(f32::to_bits);
        let object_origin = [10.5f32, 3.25, -7.0].map(f32::to_bits);
        let input = || NoninstancedGeometryInput {
            group,
            positions: &positions,
            normals: &normals,
            weights: &weights,
            tangents,
            material_rows,
            wind,
            object_origin,
        };
        let ours = prepare_noninstanced_geometry(input(), usize::MAX).unwrap();
        let Ok(Prepared::Ripple(expected)) = shared::noninstanced(
            &noninstanced::DeformationInputs {
                group: u32::from(group),
                wind,
                material: material_rows,
                origin: object_origin,
                positions: &positions,
                normals: &normals,
                weights: &weights,
                tangents,
            },
            noninstanced::DEFAULT_MAX_VERTICES,
        ) else {
            panic!("shared group {group} deformation refused");
        };
        assert_eq!(ours.group, group);
        assert_eq!(ours.positions, expected.positions);
        assert_eq!(ours.normals, expected.normals);
        assert_eq!(ours.weights, expected.weights);
        assert_eq!(ours.tangents, expected.tangents);
        // The byte budget (48 bytes per vertex, 64 with tangents) refuses first.
        let need = 37 * if group == 4 { 64 } else { 48 };
        assert_eq!(
            validate_noninstanced_geometry(&input(), need - 1),
            Err(NoninstancedGeometryError::Budget)
        );
        assert_eq!(validate_noninstanced_geometry(&input(), need), Ok(()));
    }
    let positions = bank(4, 1, 1.0);
    let short = bank(3, 2, 0.0);
    let mut input = NoninstancedGeometryInput {
        group: 3,
        positions: &positions,
        normals: &short,
        weights: &positions,
        tangents: None,
        material_rows: [[0; 4]; 3],
        wind: [0; 5],
        object_origin: [0; 3],
    };
    assert_eq!(
        validate_noninstanced_geometry(&input, usize::MAX),
        Err(NoninstancedGeometryError::Population(2))
    );
    input.normals = &positions;
    input.group = 4;
    assert_eq!(
        validate_noninstanced_geometry(&input, usize::MAX),
        Err(NoninstancedGeometryError::MissingTangent)
    );
    input.group = 5;
    assert_eq!(
        validate_noninstanced_geometry(&input, usize::MAX),
        Err(NoninstancedGeometryError::Group(5))
    );
    // A non-finite word is the deformation's own refusal, after validation.
    input.group = 3;
    let mut bad = positions.clone();
    bad[1][0] = f32::NAN.to_bits();
    input.positions = &bad;
    assert_eq!(validate_noninstanced_geometry(&input, usize::MAX), Ok(()));
    assert!(matches!(
        prepare_noninstanced_geometry(input, usize::MAX),
        Err(NoninstancedGeometryError::Deform(_))
    ));
}

fn palette(matrices: usize) -> Vec<u8> {
    let mut bytes = Vec::new();
    for m in 0..matrices {
        let rows: [[f32; 4]; 3] = [
            [1.0, 0.0, 0.0, m as f32],
            [0.0, 1.0, 0.0, 0.5],
            [0.0, 0.0, 1.0, -0.25],
        ];
        for value in rows.iter().flatten() {
            bytes.extend_from_slice(&value.to_be_bytes());
        }
    }
    bytes
}

#[test]
fn skinning_site_reads_split_palette_segments_like_the_shared_entry_point() {
    let positions = bank(9, 5, 1.0);
    let normals = bank(9, 6, 0.0);
    let tangents = bank(9, 7, 1.0);
    let joined = palette(3);
    // The second matrix crosses from the first segment into the second.
    let segments = [&joined[..70], &joined[70..]];
    let influences: Vec<u8> = (0..9u8)
        .flat_map(|v| [255, v % 3, 0, 0, 0, 0, 0, 0])
        .collect();
    let object_pose = [0f32, 0.0, 0.0, 1.0, 2.0, -1.0, 0.5, 1.0].map(f32::to_bits);
    let ours = prepare_skinning(
        SkinningInput {
            positions: &positions,
            normals: &normals,
            tangents: &tangents,
            segments,
            influences: &influences,
            object_pose,
        },
        usize::MAX,
    )
    .unwrap_or_else(|e| panic!("skinning refused: {e:?}"));
    let floats =
        |b: &[[u32; 4]]| -> Vec<[f32; 4]> { b.iter().map(|r| r.map(f32::from_bits)).collect() };
    let (p, n, t) = (floats(&positions), floats(&normals), floats(&tangents));
    let Ok(Prepared::Skinned(expected)) = shared::skinned(
        &skinning::SkinningInputs {
            segments,
            influences: &influences,
            positions: &p,
            normals: &n,
            tangents: &t,
            object: object_pose,
        },
        skinning::DEFAULT_MAX_VERTICES,
    ) else {
        panic!("shared skinning refused");
    };
    let words = |b: Vec<[f32; 4]>| -> Vec<[u32; 4]> {
        b.into_iter().map(|r| r.map(f32::to_bits)).collect()
    };
    assert_eq!(ours.positions, words(expected.positions));
    assert_eq!(ours.normals, words(expected.normals));
    assert_eq!(ours.tangents, words(expected.tangents));
    // 48 bytes per vertex of retained output.
    assert!(matches!(
        prepare_skinning(
            SkinningInput {
                positions: &positions,
                normals: &normals,
                tangents: &tangents,
                segments,
                influences: &influences,
                object_pose,
            },
            9 * 48 - 1,
        ),
        Err(SkinningError::Budget)
    ));
    // A matrix index past the palette is the skinning's own refusal.
    let mut out_of_range = influences.clone();
    out_of_range[1] = 3;
    assert!(matches!(
        prepare_skinning(
            SkinningInput {
                positions: &positions,
                normals: &normals,
                tangents: &tangents,
                segments,
                influences: &out_of_range,
                object_pose,
            },
            usize::MAX,
        ),
        Err(SkinningError::Skin(skinning::SkinningError::Joint { .. }))
    ));
}

#[test]
fn group_zero_site_expands_instances_like_the_shared_entry_point() {
    let positions = bank(5, 8, 1.0);
    let normals = bank(5, 9, 0.0);
    let weights = bank(5, 10, 0.0);
    let indices = [0u16, 1, 2, 2, 3, 4];
    // Unit-scale instances: halves for offset x, y, z, scale and four more.
    let one = 0x3c00u16;
    let instances = [
        [0, 0, 0, one, 0, 0, 0, one],
        [one, 0, 0x3800, one, 0, 0, 0, one],
        [0, one, 0, one, 0, 0, 0, one],
    ];
    let (frequency, batch) = (2u32, 2usize);
    let pool_length = required_color_population(5, 3, batch, frequency).unwrap();
    assert_eq!(
        pool_length as u64,
        group_zero::required_pool_length(3, 5, 2, 2)
    );
    let pool: Vec<[u8; 4]> = (0..pool_length as u8)
        .map(|i| [i, 2 * i, 255, 128])
        .collect();
    let wind = [0.5f32, 0.0, 0.25, 0.75, 3.5].map(f32::to_bits);
    let material_rows = [bits([0.2, 0.1, 0.0, 0.0]), bits([0.3, 0.6, 0.0, 0.0])];
    let object_pose = [0f32, 0.0, 0.0, 1.0, 1.0, 2.0, 3.0, 1.0].map(f32::to_bits);
    let environment = [
        bits([1.0, 0.0, 0.0, 0.0]),
        bits([0.0, 1.0, 0.0, 0.0]),
        bits([0.0, 0.0, 1.0, 0.0]),
        bits([0.0, 0.0, 0.0, 1.0]),
    ];
    let fade = [0x3c00, 0x4000];
    let input = || GroupZeroGeometryInput {
        positions: &positions,
        normals: &normals,
        weights: &weights,
        indices: &indices,
        instances: &instances,
        colour_pool: &pool,
        color_frequency: frequency,
        batch_maximum: batch,
        object_pose,
        source_view_rows: environment,
        fade,
        material_rows,
        wind,
    };
    let ours = prepare_group_zero_geometry(input(), usize::MAX).unwrap();
    let Ok(Prepared::GroupZero(expected)) = shared::group_zero(
        &group_zero::GroupZeroInputs {
            wind,
            material: material_rows,
            object: object_pose,
            fade,
            environment,
            instances: &instances,
            colour_frequency: frequency,
            batch_max: batch as u32,
            colour_pool: &pool,
            positions: &positions,
            normals: &normals,
            weights: &weights,
            indices: &indices,
        },
        group_zero::DEFAULT_MAX_VERTICES,
    ) else {
        panic!("shared group 0 deformation refused");
    };
    assert_eq!(ours.positions, expected.positions);
    assert_eq!(ours.attribute12, expected.colours);
    assert_eq!(ours.indices, expected.indices);
    assert_eq!((ours.instance_population, ours.base_vertices), (3, 5));
    // 32 bytes per expanded vertex plus 4 per expanded index.
    let need = 3 * 5 * 32 + 3 * indices.len() * 4;
    assert!(matches!(
        prepare_group_zero_geometry(input(), need - 1),
        Err(GroupZeroGeometryError::Budget)
    ));
    assert!(prepare_group_zero_geometry(input(), need).is_ok());
    assert_eq!(
        required_color_population(5, 3, batch, 1),
        Err(GroupZeroGeometryError::ColorFrequency(1))
    );
    assert_eq!(
        required_color_population(5, 3, 0, frequency),
        Err(GroupZeroGeometryError::BatchMaximum)
    );
}
