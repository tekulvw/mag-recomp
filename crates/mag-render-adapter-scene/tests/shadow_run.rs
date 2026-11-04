use mag_render_adapter_scene::{
    depth_sources::*, error::Error, geometry_attributes::OwnedPrimaryAttributes,
};
use mag_render_adapter_scene::{
    depth_sources::{DepthImmutablePartition, DepthItemRoute},
    mesh_sources::GeometrySource,
    shadow_run::*,
};
use mag_render_behavior::{
    setup::{CullFace, FrontFace, PipelineState},
    shadow::{LayoutAssumptions, ShadowError, ShadowQuestion},
};
use std::{collections::BTreeMap, sync::Arc};

fn transform(seed: u32) -> [u8; 112] {
    let mut t = [0u8; 112];
    for i in 0..16 {
        t[32 + 4 * i..36 + 4 * i].copy_from_slice(&(seed + i as u32).to_be_bytes());
    }
    t
}
fn partition(n: usize) -> DepthImmutablePartition {
    DepthImmutablePartition {
        source_address: 0x1000,
        positions: Ok(Arc::new(vec![[0x3f80_0000, 0, 0, 0x3f80_0000]; n])),
        indices: Ok(Arc::new((0..n as u16).collect())),
        attributes: None,
        record: Box::new([0; 544]),
        secondary_attributes: None,
        skinning: None,
    }
}
struct Fixture {
    transforms: Vec<[u8; 112]>,
    partitions: Vec<Vec<DepthImmutablePartition>>,
}
fn fixture(n: usize) -> Fixture {
    Fixture {
        transforms: (0..n as u32).map(|i| transform(0x100 * (i + 1))).collect(),
        partitions: (0..n).map(|i| vec![partition(3 + i)]).collect(),
    }
}
fn caster<'a>(f: &'a Fixture, i: usize, census: u32, mesh_group: u16) -> CasterSource<'a> {
    CasterSource {
        census_group: census,
        route: DepthItemRoute::Prepare,
        mesh_group: Some(mesh_group),
        geometry: Some(GeometrySource::StaticPartitions {
            first: 0x2000,
            count: 1,
        }),
        transform: Some(&f.transforms[i]),
        partitions: Some(&f.partitions[i]),
        mbi: &NO_MBI,
        material: None,
    }
}
static NO_MBI: [u8; 32] = [0; 32];
const MASK: u16 = 0xfe;

#[test]
fn enabled_casters_draw_in_cohort_order_with_main_transform_rule_and_pass_state() {
    let f = fixture(4);
    // Group 0 is not enabled by the mask; groups 1, 2 and 5 are.
    let sources = [
        caster(&f, 0, 0, 1),
        caster(&f, 1, 1, 1),
        caster(&f, 2, 2, 2),
        caster(&f, 3, 5, 5),
    ];
    let plan = plan_shadow_casters(
        0,
        MASK,
        &sources,
        &[],
        &PipelineState::default(),
        &LayoutAssumptions::default(),
        &ShadowRunPolicy::default(),
    )
    .unwrap();
    assert_eq!(
        plan.draws.iter().map(|d| d.item).collect::<Vec<_>>(),
        [1, 2, 3]
    );
    let d = &plan.draws[0];
    assert_eq!(d.object_to_clip[0], [0x200, 0x201, 0x202, 0x203]);
    assert_eq!(d.object_to_clip[3], [0x20c, 0x20d, 0x20e, 0x20f]);
    assert_eq!(d.partitions[0].positions.len(), 4);
    assert_eq!(d.state.cull_enable, Some(true));
    assert_eq!(d.state.cull_face, Some(CullFace::Back));
    assert_eq!(d.state.front_face, Some(FrontFace::CounterClockwise));
    assert_eq!(d.state.polygon_offset_fill, Some(true));
    assert_eq!(d.state.offset_factor, Some(2.0f32.to_bits()));
    assert_eq!(d.state.offset_units, Some(60.0f32.to_bits()));
    assert_eq!(d.state.depth_write, Some(true));
    assert!(plan.pass.clear.is_some());
    assert_eq!(plan.pass.viewport.rectangle, [0, 0, 1024, 1024]);
}

#[test]
fn cascade_one_needs_the_right_half_assumption() {
    let f = fixture(1);
    let sources = [caster(&f, 0, 1, 1)];
    let refused = plan_shadow_casters(
        1,
        MASK,
        &sources,
        &[],
        &PipelineState::default(),
        &LayoutAssumptions::default(),
        &ShadowRunPolicy::default(),
    );
    assert_eq!(
        refused,
        Err(ShadowRunRefusal::Behaviour(ShadowError::Unresolved(
            ShadowQuestion::CascadeOneHalf
        )))
    );
    let assumptions = LayoutAssumptions {
        cascade_one_right_half: true,
    };
    let plan = plan_shadow_casters(
        1,
        MASK,
        &sources,
        &[],
        &PipelineState::default(),
        &assumptions,
        &ShadowRunPolicy::default(),
    )
    .unwrap();
    assert_eq!(plan.pass.viewport.rectangle, [1024, 0, 1024, 1024]);
    assert!(plan.pass.clear.is_none());
}

#[test]
fn repeats_and_terminated_items_draw_nothing() {
    let f = fixture(3);
    let mut sources = [
        caster(&f, 0, 1, 1),
        caster(&f, 1, 1, 1),
        caster(&f, 2, 2, 2),
    ];
    sources[1].route = DepthItemRoute::Repeated;
    sources[2].route = DepthItemRoute::Terminates;
    let plan = plan_shadow_casters(
        0,
        MASK,
        &sources,
        &[],
        &PipelineState::default(),
        &LayoutAssumptions::default(),
        &ShadowRunPolicy::default(),
    )
    .unwrap();
    assert_eq!(plan.draws.len(), 1);
    assert_eq!(plan.repeats, 1);
    assert_eq!(plan.pass.casters.len(), 2);
}

#[test]
fn deformation_refuses_unless_undeformed_drawing_is_allowed() {
    let f = fixture(2);
    let sources = [caster(&f, 0, 1, 1), caster(&f, 1, 3, 3)];
    let strict = ShadowRunPolicy::default();
    assert_eq!(
        plan_shadow_casters(
            0,
            MASK,
            &sources,
            &[],
            &PipelineState::default(),
            &LayoutAssumptions::default(),
            &strict
        ),
        Err(ShadowRunRefusal::Deformation)
    );
    let lenient = ShadowRunPolicy {
        allow_undeformed: true,
        ..strict
    };
    let plan = plan_shadow_casters(
        0,
        MASK,
        &sources,
        &[],
        &PipelineState::default(),
        &LayoutAssumptions::default(),
        &lenient,
    )
    .unwrap();
    assert_eq!((plan.draws.len(), plan.undeformed), (2, 1));
    assert!(plan.draws[1].undeformed && !plan.draws[0].undeformed);
}

#[test]
fn unprepared_casters_refuse_or_are_omitted_and_listed() {
    let f = fixture(4);
    let mut sources = [
        caster(&f, 0, 1, 1),
        caster(&f, 1, 1, 1),
        caster(&f, 2, 2, 2),
        caster(&f, 3, 7, 7),
    ];
    sources[0].transform = None;
    sources[1].geometry = Some(GeometrySource::DirectDraw);
    sources[2].partitions = None;
    sources[3].geometry = Some(GeometrySource::SkinnedPartitions {
        first: 0x2000,
        count: 1,
        palette_address: 0x3000,
    });
    assert_eq!(
        plan_shadow_casters(
            0,
            MASK,
            &sources,
            &[],
            &PipelineState::default(),
            &LayoutAssumptions::default(),
            &ShadowRunPolicy::default()
        ),
        Err(ShadowRunRefusal::MissingTransform)
    );
    let omit = ShadowRunPolicy {
        omit_unprepared: true,
        ..Default::default()
    };
    let plan = plan_shadow_casters(
        0,
        MASK,
        &sources,
        &[],
        &PipelineState::default(),
        &LayoutAssumptions::default(),
        &omit,
    )
    .unwrap();
    assert!(plan.draws.is_empty());
    assert_eq!(
        plan.omitted,
        vec![
            (0, ShadowRunRefusal::MissingTransform),
            (1, ShadowRunRefusal::DirectDraw),
            (2, ShadowRunRefusal::Partitions),
            (
                3,
                ShadowRunRefusal::Behaviour(ShadowError::Unresolved(
                    ShadowQuestion::GroupSevenSkinning
                ))
            ),
        ]
    );
}

#[test]
fn a_failed_stream_and_the_cull_policy() {
    let mut f = fixture(1);
    f.partitions[0][0].indices =
        Err(mag_render_adapter_scene::geometry_streams::GeometryStreamError::SourceExtent);
    let sources = [caster(&f, 0, 1, 1)];
    assert_eq!(
        plan_shadow_casters(
            0,
            MASK,
            &sources,
            &[],
            &PipelineState::default(),
            &LayoutAssumptions::default(),
            &ShadowRunPolicy::default()
        ),
        Err(ShadowRunRefusal::Partitions)
    );
    let f = fixture(1);
    let sources = [caster(&f, 0, 1, 1)];
    let off = ShadowRunPolicy {
        cull: CullPolicy::Disabled,
        ..Default::default()
    };
    let plan = plan_shadow_casters(
        0,
        MASK,
        &sources,
        &[],
        &PipelineState::default(),
        &LayoutAssumptions::default(),
        &off,
    )
    .unwrap();
    assert_eq!(plan.draws[0].state.cull_enable, Some(false));
}

#[test]
fn an_empty_mask_is_an_empty_plan_and_mesh_group_reads_the_record() {
    let f = fixture(1);
    let sources = [caster(&f, 0, 1, 1)];
    let plan = plan_shadow_casters(
        0,
        0,
        &sources,
        &[],
        &PipelineState::default(),
        &LayoutAssumptions::default(),
        &ShadowRunPolicy::default(),
    )
    .unwrap();
    assert!(plan.draws.is_empty() && plan.pass.casters.is_empty());
    let mut mesh = vec![0u8; 192];
    mesh[0xbc..0xbe].copy_from_slice(&4u16.to_be_bytes());
    assert_eq!(mesh_group(&mesh), Some(4));
    assert_eq!(mesh_group(&mesh[..0xbd]), None);
}

fn put(bytes: &mut [u8], at: usize, value: u32) {
    bytes[at..at + 4].copy_from_slice(&value.to_be_bytes());
}
fn capture_fixture() -> (DepthInput, BTreeMap<u32, Vec<u8>>) {
    let identity = DepthIdentity {
        vm: 1,
        invocation: 2,
        owner: 3,
        occurrence: 4,
        execution_update: 5,
        allocation: 6,
        job: 0x20000,
        camera: 3,
    };
    let input = DepthInput {
        identity,
        bucket: 0x21000,
        marker: 0x22000,
        pass: 0x23000,
        user: 0x24000,
    };
    let mut pass = vec![0; 688];
    put(&mut pass, 0xe4, 0x25000);
    put(&mut pass, 0xe0, 0x26000);
    pass[0x19b] = 3;
    put(&mut pass, 0x284, 0x28000);
    let mut bucket = vec![0; 0x68];
    put(&mut bucket, 4, 0x25000);
    put(&mut bucket, 12, 0x26000);
    put(&mut bucket, 0x58, 0x27000);
    let mut marker = vec![0; 320];
    put(&mut marker, 4, 2);
    put(&mut marker, 64, 2);
    let mut mbi = vec![0; 32];
    mbi[0] = 0x10;
    mbi[7] = 3;
    put(&mut mbi, 12, 0x4000);
    put(&mut mbi, 20, 1);
    put(&mut mbi, 28, 6);
    let mut mesh = vec![0; 192];
    put(&mut mesh, 0x9c, 0x1000);
    put(&mut mesh, 0x50, 0x100);
    let material = vec![0; 96];
    let transform = vec![0; 112];
    (
        input,
        BTreeMap::from([
            (input.pass, pass),
            (input.user, vec![0; 160]),
            (input.bucket, bucket),
            (input.marker, marker),
            (0x25000, mbi),
            (0x26000, transform),
            (0x27000, vec![0, 0, 0, 0]),
            (0x30000, material),
            (0x40000, mesh),
        ]),
    )
}

#[test]
fn a_captured_run_plans_through_the_depth_source_capture() {
    let (input, mut memory) = capture_fixture();
    // Mesh group 1 at +0xBC routes the item onto the static preparation.
    memory.get_mut(&0x40000).unwrap()[0xbc..0xbe].copy_from_slice(&1u16.to_be_bytes());
    let sources = copy_depth_sources(input, DepthBudget::default(), &mut |a, n| {
        memory
            .get(&a)
            .filter(|v| v.len() == n)
            .cloned()
            .ok_or(Error::OutsideSnapshot)
    })
    .unwrap();
    let inherited = PipelineState::default();
    let input = |policy| ShadowRunInput {
        cascade: 0,
        group_mask: MASK,
        sources: &sources,
        inherited: &inherited,
        assumptions: LayoutAssumptions::default(),
        policy,
    };
    // The fixture retains no partition streams, so the caster is unprepared.
    assert_eq!(
        plan_shadow_run(&input(ShadowRunPolicy::default())),
        Err(ShadowRunRefusal::Partitions)
    );
    let plan = plan_shadow_run(&input(ShadowRunPolicy {
        omit_unprepared: true,
        ..Default::default()
    }))
    .unwrap();
    // The fixture's second item repeats the first.
    assert_eq!(plan.repeats, 1);
    assert_eq!(plan.omitted, vec![(0, ShadowRunRefusal::Partitions)]);
    assert_eq!(plan.pass.casters.len(), 2);
}

fn floats(values: &[f32]) -> Vec<u8> {
    values.iter().flat_map(|v| v.to_be_bytes()).collect()
}
fn deformable(n: usize) -> DepthImmutablePartition {
    // Normals (semantic 2) and weights (semantic 9), four floats each in a
    // 32-byte vertex.
    let format = {
        let mut f = vec![2u8, 32, 0, 0, 0, 0, 0, 0];
        f.extend([0, 2, 3, 2, 16, 0, 0, 0]);
        f.extend([16, 2, 3, 9, 16, 0, 0, 0]);
        f
    };
    let mut source = Vec::new();
    for i in 0..n {
        source.extend(floats(&[0., 1., 0., 0., 0.25 * i as f32, 0.5, 0., 0.]));
    }
    let attributes = mag_render_adapter_scene::geometry_attributes::decode_primary_attributes(
        &format,
        &source,
        0x1000,
        n,
        1 << 20,
    )
    .unwrap();
    DepthImmutablePartition {
        source_address: 0x1000,
        positions: Ok(Arc::new(
            (0..n)
                .map(|i| [(i as f32).to_bits(), 0, 1f32.to_bits(), 1f32.to_bits()])
                .collect(),
        )),
        indices: Ok(Arc::new((0..n as u16).collect())),
        attributes: Some(Ok(attributes)),
        record: Box::new([0; 544]),
        secondary_attributes: None,
        skinning: None,
    }
}
fn wind_pass(mode: u32, workers: u32) -> Vec<u8> {
    let mut pass = vec![0u8; 688];
    pass[0x230..0x234].copy_from_slice(&mode.to_be_bytes());
    pass[0x22c..0x230].copy_from_slice(&workers.to_be_bytes());
    for (i, v) in [0.5f32, 0.25, 1.0, 0.125, 2.0].into_iter().enumerate() {
        pass[0x260 + 4 * i..0x264 + 4 * i].copy_from_slice(&v.to_be_bytes());
    }
    pass
}

#[test]
fn groups_three_and_four_deform_through_the_shared_entry_point() {
    let partitions = vec![deformable(4)];
    let transform = transform(0x100);
    let material: Vec<u8> = (0..24)
        .flat_map(|i| (0.1f32 * i as f32).to_be_bytes())
        .collect();
    let pass = wind_pass(8, 2);
    let source = CasterSource {
        census_group: 3,
        route: DepthItemRoute::Prepare,
        mesh_group: Some(3),
        geometry: Some(GeometrySource::StaticPartitions {
            first: 0x2000,
            count: 1,
        }),
        transform: Some(&transform),
        partitions: Some(&partitions),
        mbi: &NO_MBI,
        material: Some(&material),
    };
    let plan = plan_shadow_casters(
        0,
        MASK,
        &[source],
        &pass,
        &PipelineState::default(),
        &LayoutAssumptions::default(),
        &ShadowRunPolicy::default(),
    )
    .unwrap();
    assert_eq!(
        (plan.draws.len(), plan.deformed, plan.undeformed),
        (1, 1, 0)
    );
    let d = &plan.draws[0];
    assert!(d.deformed && !d.undeformed);
    // Exactly the shared entry point's positions for the same inputs.
    let word = |b: &[u8], at: usize| u32::from_be_bytes(b[at..at + 4].try_into().unwrap());
    let bank = |semantic| -> Vec<[u32; 4]> {
        let attributes = partitions[0].attributes.as_ref().unwrap().as_ref().unwrap();
        attributes
            .first_semantic(semantic)
            .unwrap()
            .unwrap()
            .words
            .to_vec()
    };
    let positions = partitions[0].positions.as_ref().unwrap();
    let (normals, weights) = (bank(2), bank(9));
    let expected = mag_render_adapter_scene::deformation::prepare_noninstanced_geometry(
        mag_render_adapter_scene::deformation::NoninstancedGeometryInput {
            group: 3,
            positions,
            normals: &normals,
            weights: &weights,
            tangents: None,
            material_rows: core::array::from_fn(|r| {
                core::array::from_fn(|c| word(&material, r * 16 + c * 4))
            }),
            wind: core::array::from_fn(|i| word(&pass, 0x260 + 4 * i)),
            object_origin: core::array::from_fn(|i| word(&transform, 16 + 4 * i)),
        },
        DEFORMATION_BUDGET,
    )
    .unwrap();
    assert_eq!(*d.partitions[0].positions, expected.positions);
    // A pass mode outside Main's admitted modes refuses the caster.
    let other = wind_pass(4, 2);
    assert_eq!(
        plan_shadow_casters(
            0,
            MASK,
            &[source],
            &other,
            &PipelineState::default(),
            &LayoutAssumptions::default(),
            &ShadowRunPolicy::default()
        ),
        Err(ShadowRunRefusal::Deformation)
    );
    // So does a group-4 caster without a tangent bank.
    let four = CasterSource {
        census_group: 4,
        mesh_group: Some(4),
        ..source
    };
    assert_eq!(
        plan_shadow_casters(
            0,
            MASK,
            &[four],
            &pass,
            &PipelineState::default(),
            &LayoutAssumptions::default(),
            &ShadowRunPolicy::default()
        ),
        Err(ShadowRunRefusal::Deformation)
    );
}

fn content(
    address: u32,
    bytes: &[u8],
) -> Arc<mag_render_adapter_scene::shared_scene_sources::SourceContent> {
    mag_render_adapter_scene::shared_scene_sources::SourceSnapshot::default()
        .observe(
            mag_render_adapter_scene::shared_scene_sources::SourceDomain::Guest,
            address,
            bytes,
        )
        .unwrap()
}
fn attributes(semantics: &[u8], n: usize, seed: f32) -> OwnedPrimaryAttributes {
    // One four-float bank per semantic, interleaved in the vertex.
    let stride = 16 * semantics.len();
    let mut format = vec![semantics.len() as u8, stride as u8, 0, 0, 0, 0, 0, 0];
    for (i, &semantic) in semantics.iter().enumerate() {
        format.extend([16 * i as u8, 2, 3, semantic, 16, 0, 0, 0]);
    }
    let mut source = Vec::new();
    for v in 0..n {
        for i in 0..semantics.len() {
            source.extend(floats(&[seed + i as f32, 0.5 * v as f32, 1.0, 0.0]));
        }
    }
    mag_render_adapter_scene::geometry_attributes::decode_primary_attributes(
        &format,
        &source,
        0x1000,
        n,
        1 << 20,
    )
    .unwrap()
}
fn skinnable(n: usize) -> DepthImmutablePartition {
    let mut palette = Vec::new();
    for m in 0..2 {
        for value in [
            1.0f32, 0.0, 0.0, m as f32, 0.0, 1.0, 0.0, 0.5, 0.0, 0.0, 1.0, -0.25,
        ] {
            palette.extend(value.to_be_bytes());
        }
    }
    let mut config = vec![0u8; 16];
    config[0] = 0x83;
    config[6] = 17;
    config[12..16].copy_from_slice(&[255; 4]);
    let influences: Vec<u8> = (0..n as u8)
        .flat_map(|v| [255, v % 2, 0, 0, 0, 0, 0, 0])
        .collect();
    let mut record = [0u8; 544];
    record[0x30 + 8 * 3..0x34 + 8 * 3].copy_from_slice(&60u32.to_be_bytes());
    record[0x30 + 8 * 4..0x34 + 8 * 4].copy_from_slice(&36u32.to_be_bytes());
    DepthImmutablePartition {
        source_address: 0x1000,
        record: Box::new(record),
        positions: Ok(Arc::new(
            (0..n)
                .map(|i| [(i as f32).to_bits(), 0, 1f32.to_bits(), 1f32.to_bits()])
                .collect(),
        )),
        indices: Ok(Arc::new((0..n as u16).collect())),
        // Normals in the primary banks, tangents only in the secondary banks.
        attributes: Some(Ok(attributes(&[2], n, 0.0))),
        secondary_attributes: Some(Ok(attributes(&[3], n, 1.0))),
        skinning: Some(Ok(DepthSkinningSource {
            config: content(0x5000, &config),
            palette_addresses: [0x6000, 0x603c],
            segments: [
                content(0x6000, &palette[..60]),
                content(0x603c, &palette[60..]),
            ],
            influences: content(0x7000, &influences),
        })),
    }
}

#[test]
fn group_seven_skins_through_the_shared_entry_point_only_when_admitted() {
    let partitions = vec![skinnable(4)];
    let mut transform = transform(0x100);
    for (i, v) in [0f32, 0.0, 0.0, 1.0, 2.0, -1.0, 0.5, 1.0]
        .into_iter()
        .enumerate()
    {
        transform[4 * i..4 * i + 4].copy_from_slice(&v.to_be_bytes());
    }
    let material = vec![0u8; 96];
    let pass = wind_pass(8, 2);
    let source = CasterSource {
        census_group: 7,
        route: DepthItemRoute::Prepare,
        mesh_group: Some(7),
        geometry: Some(GeometrySource::SkinnedPartitions {
            first: 0x2000,
            count: 1,
            palette_address: 0x6000,
        }),
        transform: Some(&transform),
        partitions: Some(&partitions),
        mbi: &NO_MBI,
        material: Some(&material),
    };
    let plan = |source: CasterSource<'_>, pass: &[u8], admit: bool| {
        plan_shadow_casters(
            0,
            MASK,
            &[source],
            pass,
            &PipelineState::default(),
            &LayoutAssumptions::default(),
            &ShadowRunPolicy {
                admit_group_seven: admit,
                ..ShadowRunPolicy::default()
            },
        )
    };
    // Refused by default, as the behaviour crate leaves it open.
    assert_eq!(
        plan(source, &pass, false),
        Err(ShadowRunRefusal::Behaviour(ShadowError::Unresolved(
            ShadowQuestion::GroupSevenSkinning
        )))
    );
    let admitted = plan(source, &pass, true).unwrap();
    assert_eq!(
        (admitted.draws.len(), admitted.skinned, admitted.deformed),
        (1, 1, 0)
    );
    assert!(admitted.draws[0].skinned && !admitted.draws[0].undeformed);
    // Exactly the shared entry point's positions for the same inputs.
    let p = &partitions[0];
    let bank = |a: &OwnedPrimaryAttributes, semantic| {
        a.first_semantic(semantic).unwrap().unwrap().words.to_vec()
    };
    let normals = bank(p.attributes.as_ref().unwrap().as_ref().unwrap(), 2);
    let tangents = bank(
        p.secondary_attributes.as_ref().unwrap().as_ref().unwrap(),
        3,
    );
    let s = p.skinning.as_ref().unwrap().as_ref().unwrap();
    let expected = mag_render_adapter_scene::deformation::prepare_skinning(
        mag_render_adapter_scene::deformation::SkinningInput {
            positions: p.positions.as_ref().unwrap(),
            normals: &normals,
            tangents: &tangents,
            segments: [s.segments[0].bytes(), s.segments[1].bytes()],
            influences: s.influences.bytes(),
            object_pose: core::array::from_fn(|i| {
                u32::from_be_bytes(transform[4 * i..4 * i + 4].try_into().unwrap())
            }),
        },
        DEFORMATION_BUDGET,
    )
    .unwrap();
    assert_eq!(
        *admitted.draws[0].partitions[0].positions,
        expected.positions
    );
    assert_ne!(expected.positions, **p.positions.as_ref().unwrap());

    // Admitted but unqualified casters refuse; none is drawn unskinned.
    assert_eq!(
        plan(source, &wind_pass(0x28, 1), true),
        Err(ShadowRunRefusal::Skinning)
    );
    let mut instanced = transform;
    instanced[0x6f] = 1;
    assert_eq!(
        plan(
            CasterSource {
                transform: Some(&instanced),
                ..source
            },
            &pass,
            true
        ),
        Err(ShadowRunRefusal::Skinning)
    );
    let mut bare = skinnable(4);
    bare.skinning = None;
    let bare = vec![bare];
    assert_eq!(
        plan(
            CasterSource {
                partitions: Some(&bare),
                ..source
            },
            &pass,
            true
        ),
        Err(ShadowRunRefusal::Skinning)
    );
    // Only group 7 is admitted; other skinned groups stay refused.
    assert!(matches!(
        plan(
            CasterSource {
                census_group: 5,
                mesh_group: Some(5),
                ..source
            },
            &pass,
            true
        ),
        Err(ShadowRunRefusal::Behaviour(ShadowError::ItemRefused(_)))
    ));
}

#[test]
fn a_group_three_record_with_palette_controls_is_not_deformed() {
    let clean = vec![deformable(4)];
    let mut partition = deformable(4);
    partition.record[0x30 + 8 * 3 + 3] = 48;
    let partitions = vec![partition];
    let transform = transform(0x100);
    let material = vec![0u8; 96];
    let source = CasterSource {
        census_group: 3,
        route: DepthItemRoute::Prepare,
        mesh_group: Some(3),
        geometry: Some(GeometrySource::StaticPartitions {
            first: 0x2000,
            count: 1,
        }),
        transform: Some(&transform),
        partitions: Some(&partitions),
        mbi: &NO_MBI,
        material: Some(&material),
    };
    assert_eq!(
        plan_shadow_casters(
            0,
            MASK,
            &[source],
            &wind_pass(8, 2),
            &PipelineState::default(),
            &LayoutAssumptions::default(),
            &ShadowRunPolicy::default()
        ),
        Err(ShadowRunRefusal::Deformation)
    );
    // The same caster without the palette controls deforms.
    let plan = plan_shadow_casters(
        0,
        MASK,
        &[CasterSource {
            partitions: Some(&clean),
            ..source
        }],
        &wind_pass(8, 2),
        &PipelineState::default(),
        &LayoutAssumptions::default(),
        &ShadowRunPolicy::default(),
    )
    .unwrap();
    assert_eq!(plan.deformed, 1);
}
