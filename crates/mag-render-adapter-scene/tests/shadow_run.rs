use mag_render_adapter_scene::{depth_sources::*, error::Error};
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
    }
}
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
