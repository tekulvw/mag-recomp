//! The opt-in GPU group-zero path: kernel translation, the binary32 reference of the kernel's
//! arithmetic against the behaviour crate's CPU deformation within the specification's tolerance,
//! the layout plan and every refusal. Every input is synthetic.

use mag_render_behavior::group_zero::{GroupZeroError, GroupZeroInputs, position_tolerance};
use mag_render_behavior::vertex_inputs::{
    AttributeList, AuxiliaryAttribute, AuxiliarySet, GroupZeroPreparation, Prepared, VertexBinding,
    VertexDraw, pack_vertex_inputs,
};
use mag_render_behavior_glue::abi::{LANGUAGE_HLSL, LANGUAGE_MSL, LANGUAGE_SPIRV};
use mag_render_behavior_glue::deformation::{
    DeformationKey, DeformationStore, GroupZeroLayouts, Prepared as Deformed, group_zero,
    group_zero_preferring_gpu,
};
use mag_render_behavior_glue::gpu_group_zero::{self, GpuRefusal, STRIDE, kernel_program};

/// The rebased half of an exactly representable value.
fn half(value: f32) -> u16 {
    if value == 0.0 {
        return 0;
    }
    let bits = value.to_bits();
    assert_eq!(bits & 0x1fff, 0);
    (((bits >> 16) & 0x8000) | ((((bits >> 23) & 0xff) - 112) << 10) | ((bits >> 13) & 0x3ff))
        as u16
}

struct Rng(u64);
impl Rng {
    fn next(&mut self) -> u64 {
        self.0 = self.0.wrapping_add(0x9e37_79b9_7f4a_7c15);
        let mut z = self.0;
        z = (z ^ (z >> 30)).wrapping_mul(0xbf58_476d_1ce4_e5b9);
        z = (z ^ (z >> 27)).wrapping_mul(0x94d0_49bb_1331_11eb);
        z ^ (z >> 31)
    }
    fn unit(&mut self) -> f32 {
        (self.next() >> 40) as f32 / (1u64 << 24) as f32
    }
    fn range(&mut self, lo: f32, hi: f32) -> f32 {
        lo + (hi - lo) * self.unit()
    }
    fn pick(&mut self, values: &[f32]) -> f32 {
        values[(self.next() % values.len() as u64) as usize]
    }
}

struct Item {
    positions: Vec<[u32; 4]>,
    normals: Vec<[u32; 4]>,
    weights: Vec<[u32; 4]>,
    indices: Vec<u16>,
    instances: Vec<[u16; 8]>,
    pool: Vec<[u8; 4]>,
    wind: [u32; 5],
    material: [[u32; 4]; 2],
    object: [u32; 8],
    environment: [[u32; 4]; 4],
}

fn bits(values: [f32; 4]) -> [u32; 4] {
    values.map(f32::to_bits)
}

/// A synthetic wind item: `vertices` source vertices, `instances` instances.
fn item(seed: u64, vertices: usize, instances: usize, phase: f32) -> Item {
    let mut rng = Rng(seed);
    let halves = [0.25, 0.5, -0.5, 0.75, 1.0, -1.25, 1.5, 2.0, -3.0, 4.5];
    let mut item = Item {
        positions: (0..vertices)
            .map(|_| {
                bits([
                    rng.range(-2.0, 2.0),
                    rng.range(0.0, 3.0),
                    rng.range(-2.0, 2.0),
                    1.0,
                ])
            })
            .collect(),
        normals: (0..vertices)
            .map(|_| {
                let (x, y) = (rng.range(-1.0, 1.0), rng.range(-1.0, 1.0));
                bits([x, y, rng.range(-1.0, 1.0), 0.0])
            })
            .collect(),
        weights: (0..vertices)
            .map(|_| bits([0.0, 0.0, 0.0, rng.unit()]))
            .collect(),
        indices: (0..3 * vertices as u16)
            .map(|i| i % vertices as u16)
            .collect(),
        instances: (0..instances)
            .map(|_| {
                std::array::from_fn(|lane| match lane {
                    0..=2 => half(rng.pick(&halves) * 8.0),
                    3 => half(rng.pick(&[0.5, 1.0, 1.5, 2.0])),
                    _ => half(rng.pick(&halves)),
                })
            })
            .collect(),
        pool: Vec::new(),
        wind: [1.3, 0.0, 0.7, 0.4, phase].map(f32::to_bits),
        material: [bits([0.8, 0.6, 0.0, 0.0]), bits([1.7, 0.0, 0.0, 0.0])],
        object: [0.0, 0.0, 0.0, 1.0, 10.0, -4.0, 2.5, 1.0].map(f32::to_bits),
        environment: [
            bits([1.0, 0.0, 0.0, 0.0]),
            bits([0.0, 1.0, 0.0, 0.0]),
            bits([0.0, 0.0, 1.0, 0.0]),
            bits([0.0, 0.0, 0.0, 1.0]),
        ],
    };
    let entries = vertices * instances;
    item.pool = (0..entries)
        .map(|_| {
            let w = rng.next();
            [w as u8, (w >> 8) as u8, (w >> 16) as u8, (w >> 24) as u8]
        })
        .collect();
    item
}

impl Item {
    fn inputs(&self) -> GroupZeroInputs<'_> {
        GroupZeroInputs {
            wind: self.wind,
            material: self.material,
            object: self.object,
            fade: [0, 0],
            environment: self.environment,
            instances: &self.instances,
            colour_frequency: 3,
            batch_max: 4,
            colour_pool: &self.pool,
            positions: &self.positions,
            normals: &self.normals,
            weights: &self.weights,
            indices: &self.indices,
        }
    }
}

const MAX: u64 = 1 << 22;

/// The largest position error of the kernel reference against the CPU deformation, absolute and
/// as a fraction of the per-lane tolerance.
fn compare(item: &Item) -> (f64, f64) {
    let inputs = item.inputs();
    let cpu = mag_render_behavior::group_zero::deform_group_zero(&inputs, MAX).unwrap();
    let work = gpu_group_zero::prepare(&inputs, MAX).unwrap();
    let rows = gpu_group_zero::reference(&work);
    let validated = gpu_group_zero::validated(&inputs).unwrap();
    assert_eq!(work.indices, cpu.indices);
    assert_eq!(rows.len(), 2 * cpu.positions.len());
    let vertices = item.positions.len();
    let (mut worst, mut ratio) = (0f64, 0f64);
    for (index, (position, colour)) in cpu.positions.iter().zip(&cpu.colours).enumerate() {
        let (instance, vertex) = (index / vertices, index % vertices);
        let gpu = rows[2 * index];
        assert_eq!(gpu[3].to_bits(), position[3], "w lane");
        for lane in 0..3 {
            let error = (gpu[lane] as f64 - f32::from_bits(position[lane]) as f64).abs();
            let tolerance = position_tolerance(
                &validated,
                &validated.placements[instance],
                item.positions[vertex],
                item.normals[vertex],
                item.weights[vertex],
                lane,
            );
            assert!(
                error <= tolerance,
                "instance {instance} vertex {vertex} lane {lane}: {error} > {tolerance}"
            );
            worst = worst.max(error);
            ratio = ratio.max(error / tolerance);
        }
        assert_eq!(rows[2 * index + 1].map(f32::to_bits), *colour, "colour row");
    }
    (worst, ratio)
}

#[test]
fn kernel_reference_matches_the_cpu_deformation_within_tolerance() {
    for (seed, vertices, instances, phase) in [
        (1, 7, 5, 0.0),
        (2, 33, 17, 12.5),
        (3, 64, 9, -3.25),
        (4, 5, 40, 1.0e4),
    ] {
        let (worst, ratio) = compare(&item(seed, vertices, instances, phase));
        println!("seed {seed}: max abs error {worst:e}, max error/tolerance {ratio:.4}");
        assert!(ratio <= 1.0);
    }
}

#[test]
fn kernel_translations_and_program_record() {
    for language in [LANGUAGE_MSL, LANGUAGE_HLSL, LANGUAGE_SPIRV] {
        let program = kernel_program(language).unwrap();
        assert_eq!((program.stage, program.threads), (2, [64, 1, 1]));
        assert_eq!(program.entry, b"group_zero");
        assert_eq!(program.buffers.len(), 5);
        // Bindings 1–4 are runtime arrays sized through indices 0–3; binding 4 is the output.
        assert_eq!(
            program
                .buffers
                .iter()
                .map(|b| (b.slot, b.access, b.runtime_size_index))
                .collect::<Vec<_>>(),
            vec![(0, 1, u32::MAX), (1, 1, 0), (2, 1, 1), (3, 1, 2), (4, 2, 3)]
        );
        assert_eq!(program.buffers[0].binding_kind, 1);
        assert_eq!(program.buffers[0].minimum_bytes, 1088);
    }
    let spirv = kernel_program(LANGUAGE_SPIRV).unwrap().source;
    assert_eq!(&spirv[..4], &0x0723_0203u32.to_le_bytes());
    assert!(gpu_group_zero::kernel::MSL.contains("group_zero"));
    assert!(gpu_group_zero::kernel::HLSL.contains("group_zero"));
    assert_eq!(kernel_program(9), Err(GpuRefusal::Language(9)));
}

#[test]
fn work_encoding_and_grid() {
    let item = item(5, 10, 3, 2.0);
    let work = gpu_group_zero::prepare(&item.inputs(), MAX).unwrap();
    assert_eq!(work.vertex_count, 30);
    assert_eq!(work.groups, [1, 1, 1]);
    assert_eq!(work.output_bytes(), 30 * STRIDE as u64);
    assert_eq!(work.parameters.len(), gpu_group_zero::PARAMETER_BYTES);
    assert_eq!(work.instances.len(), 3 * gpu_group_zero::INSTANCE_BYTES);
    assert_eq!(
        work.vertices.len(),
        10 * gpu_group_zero::SOURCE_VERTEX_BYTES
    );
    // Source words are copied bit for bit; w carries the weight lane 3.
    assert_eq!(&work.vertices[..4], &item.positions[0][0].to_le_bytes());
    assert_eq!(&work.vertices[12..16], &item.weights[0][3].to_le_bytes());
    // Many vertices: a two-dimensional grid within the per-dimension limit.
    let big = self::item(6, 1000, 5000, 0.5);
    let work = gpu_group_zero::prepare(&big.inputs(), 1 << 23).unwrap();
    let [x, y, z] = work.groups;
    assert_eq!(z, 1);
    assert!(x <= gpu_group_zero::MAX_GROUPS && y > 1);
    assert!(x as u64 * y as u64 * 64 >= 5_000_000);
    assert_eq!(
        u32::from_le_bytes(work.parameters[16..20].try_into().unwrap()),
        x * 64
    );
}

#[test]
fn refusals_fall_back_to_the_cpu_path() {
    let base = item(7, 6, 4, 1.0);
    // A refusal the CPU path also makes.
    let mut short = item(7, 6, 4, 1.0);
    short.pool.truncate(1);
    assert_eq!(
        gpu_group_zero::prepare(&short.inputs(), MAX),
        Err(GpuRefusal::Deformation(GroupZeroError::Source))
    );
    // A non-finite wind term: the CPU path decides the outcome.
    let mut nan = item(7, 6, 4, 1.0);
    nan.wind[0] = f32::NAN.to_bits();
    assert!(matches!(
        gpu_group_zero::prepare(&nan.inputs(), MAX),
        Err(GpuRefusal::NonFinite(_))
    ));
    let mut bad_normal = item(7, 6, 4, 1.0);
    bad_normal.normals[2][1] = f32::INFINITY.to_bits();
    assert_eq!(
        gpu_group_zero::prepare(&bad_normal.inputs(), MAX),
        Err(GpuRefusal::NonFinite("source vertex"))
    );
    // Positions that could leave the binary32 range.
    let mut huge = item(7, 6, 4, 1.0);
    huge.positions[0] = bits([3.0e38, 0.0, 0.0, 1.0]);
    assert_eq!(
        gpu_group_zero::prepare(&huge.inputs(), MAX),
        Err(GpuRefusal::Magnitude)
    );
    // No vertices.
    let empty = Item {
        positions: Vec::new(),
        normals: Vec::new(),
        weights: Vec::new(),
        indices: Vec::new(),
        ..item(7, 6, 4, 1.0)
    };
    assert_eq!(
        gpu_group_zero::prepare(&empty.inputs(), MAX),
        Err(GpuRefusal::Grid)
    );

    // The store-level choice: off, accepted, refused.
    let outputs = [VertexBinding {
        declaration: 0,
        semantic: 1,
        slot: 0,
        format: 2,
        components: 3,
    }];
    let auxiliary = AuxiliarySet::default();
    let layouts = GroupZeroLayouts {
        outputs: &outputs,
        auxiliary: &auxiliary,
        required: &[0x0001, 0x1001],
    };
    let (off, refusal) = group_zero_preferring_gpu(&base.inputs(), MAX, false, &layouts);
    assert!(matches!(off, Ok(Deformed::GroupZero(_))) && refusal.is_none());
    let (on, refusal) = group_zero_preferring_gpu(&base.inputs(), MAX, true, &layouts);
    assert!(matches!(on, Ok(Deformed::GroupZeroGpu(_))) && refusal.is_none());
    let (fallback, refusal) = group_zero_preferring_gpu(&huge.inputs(), MAX, true, &layouts);
    assert_eq!(fallback, group_zero(&huge.inputs(), MAX));
    assert_eq!(refusal, Some(GpuRefusal::Magnitude));
    // A pass layout the rows cannot serve keeps every pass of the item on the CPU.
    let unsupported = GroupZeroLayouts {
        required: &[0x0001, 0x0101],
        ..layouts
    };
    let (fallback, refusal) = group_zero_preferring_gpu(&base.inputs(), MAX, true, &unsupported);
    assert!(matches!(fallback, Ok(Deformed::GroupZero(_))));
    assert!(matches!(refusal, Some(GpuRefusal::Layout(_))));

    // The store keeps one choice per item for every pass.
    let mut store = DeformationStore::new();
    let key = DeformationKey {
        cohort: 1,
        item: 2,
        partition: 0,
    };
    let first = store.prepare(key, || {
        group_zero_preferring_gpu(&base.inputs(), MAX, true, &layouts).0
    });
    let second = store.prepare(key, || unreachable!("prepared once"));
    assert!(std::sync::Arc::ptr_eq(&first, &second));
    assert!(matches!(*second, Ok(Deformed::GroupZeroGpu(_))));
}

#[test]
fn layout_plans_and_vertex_input_packing() {
    let item = item(8, 4, 2, 0.0);
    let work = gpu_group_zero::prepare(&item.inputs(), MAX).unwrap();
    let position = VertexBinding {
        declaration: 0,
        semantic: 1,
        slot: 0,
        format: 2,
        components: 3,
    };
    let none = AuxiliarySet::default();
    let plan = work.layout_plan(&[position], &none, 0x1001).unwrap();
    assert_eq!(
        (plan.array_mask, plan.stride, plan.fields.clone()),
        (0x1001, 32, vec![0, 12])
    );
    assert_eq!(plan.vertex_count, 8);
    let alone = work.layout_plan(&[position], &none, 0x0001).unwrap();
    assert_eq!((alone.stride, alone.fields), (32, vec![0]));

    // Packing reads the plan: no CPU vertex bytes, the produced layout's attributes.
    let colours: Vec<[u32; 4]> = Vec::new();
    let draw = VertexDraw {
        output_bindings: Some(vec![position]),
        auxiliary: Some(AuxiliarySet::default()),
        primary: AttributeList::Absent,
        secondary: AttributeList::Absent,
        positions: &[],
        raw_indices: &[],
        direct: None,
        group_zero: Prepared::Present(GroupZeroPreparation {
            positions: &[],
            indices: &work.indices,
            colours: &colours,
            auxiliary: AuxiliarySet::default(),
            gpu: Some(Ok(plan.clone())),
        }),
        non_instanced: Prepared::Absent,
    };
    let packed = pack_vertex_inputs(&draw, 0x1001, 1 << 20).unwrap();
    assert!(packed.vertices.is_empty());
    assert_eq!(
        (packed.stride, packed.attributes.clone()),
        (32, vec![(0, 0), (12, 16)])
    );
    assert_eq!(packed.indices, work.indices);
    assert_eq!(packed.produced, Some(plan));

    // Layouts the rows cannot serve.
    let refuse = |outputs: &[VertexBinding], auxiliary: &AuxiliarySet, required| {
        assert!(matches!(
            work.layout_plan(outputs, auxiliary, required),
            Err(GpuRefusal::Layout(_))
        ));
    };
    refuse(&[position], &none, 0x1000);
    refuse(&[position], &none, 0x0101);
    refuse(&[], &none, 0x0001);
    refuse(
        &[VertexBinding {
            components: 2,
            ..position
        }],
        &none,
        0x0001,
    );
    refuse(
        &[VertexBinding {
            semantic: 2,
            ..position
        }],
        &none,
        0x0001,
    );
    let rows = [[0u32; 4]; 4];
    let replaced = AuxiliarySet {
        decoded: vec![AuxiliaryAttribute {
            binding: VertexBinding {
                declaration: 1,
                semantic: 0,
                slot: 12,
                format: 2,
                components: 4,
            },
            rows: &rows,
        }],
        failed: Vec::new(),
    };
    refuse(&[position], &replaced, 0x1001);
    // A packed (CMP) position keeps three lanes.
    assert!(
        work.layout_plan(
            &[VertexBinding {
                format: 6,
                components: 1,
                ..position
            }],
            &none,
            0x0001
        )
        .is_ok()
    );
}

#[test]
fn option_is_off_unless_set_to_one() {
    // The test process does not set the option.
    if std::env::var_os(gpu_group_zero::OPTION).is_none() {
        assert!(!gpu_group_zero::enabled_by_environment());
    }
}
