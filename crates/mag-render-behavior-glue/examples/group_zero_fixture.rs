//! Writes synthetic group-zero GPU fixtures for `tools/check_group_zero_gpu.py`: the kernel's
//! MSL, its input buffers, the grid, the CPU deformation's rows and the per-lane tolerance.
//! Usage: `cargo run --example group_zero_fixture -- OUTPUT_DIRECTORY`.

use mag_render_behavior::group_zero::{GroupZeroInputs, deform_group_zero, position_tolerance};
use mag_render_behavior_glue::gpu_group_zero::{self, kernel};
use std::path::Path;

fn half(value: f32) -> u16 {
    if value == 0.0 {
        return 0;
    }
    let bits = value.to_bits();
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
    fn range(&mut self, lo: f32, hi: f32) -> f32 {
        lo + (hi - lo) * ((self.next() >> 40) as f32 / (1u64 << 24) as f32)
    }
}

fn bits(v: [f32; 4]) -> [u32; 4] {
    v.map(f32::to_bits)
}

fn write_case(root: &Path, name: &str, seed: u64, vertices: usize, instances: usize, phase: f32) {
    let mut rng = Rng(seed);
    let halves = [0.25f32, 0.5, -0.5, 0.75, 1.0, -1.25, 1.5, 2.0, -3.0, 4.5];
    let pick = |rng: &mut Rng| halves[(rng.next() % halves.len() as u64) as usize];
    let positions: Vec<[u32; 4]> = (0..vertices)
        .map(|_| {
            bits([
                rng.range(-2.0, 2.0),
                rng.range(0.0, 3.0),
                rng.range(-2.0, 2.0),
                1.0,
            ])
        })
        .collect();
    let normals: Vec<[u32; 4]> = (0..vertices)
        .map(|_| {
            bits([
                rng.range(-1.0, 1.0),
                rng.range(-1.0, 1.0),
                rng.range(-1.0, 1.0),
                0.0,
            ])
        })
        .collect();
    let weights: Vec<[u32; 4]> = (0..vertices)
        .map(|_| bits([0.0, 0.0, 0.0, rng.range(0.0, 1.0)]))
        .collect();
    let rows: Vec<[u16; 8]> = (0..instances)
        .map(|_| {
            std::array::from_fn(|lane| match lane {
                0..=2 => half(pick(&mut rng) * 8.0),
                3 => half([0.5, 1.0, 1.5, 2.0][(rng.next() % 4) as usize]),
                _ => half(pick(&mut rng)),
            })
        })
        .collect();
    let pool: Vec<[u8; 4]> = (0..vertices * instances)
        .map(|_| {
            let w = rng.next();
            [w as u8, (w >> 8) as u8, (w >> 16) as u8, (w >> 24) as u8]
        })
        .collect();
    let indices: Vec<u16> = (0..3 * vertices as u16)
        .map(|i| i % vertices as u16)
        .collect();
    let inputs = GroupZeroInputs {
        wind: [1.3f32, 0.0, 0.7, 0.4, phase].map(f32::to_bits),
        material: [bits([0.8, 0.6, 0.0, 0.0]), bits([1.7, 0.0, 0.0, 0.0])],
        object: [0.0f32, 0.0, 0.0, 1.0, 10.0, -4.0, 2.5, 1.0].map(f32::to_bits),
        fade: [0, 0],
        environment: [
            bits([1.0, 0.0, 0.0, 0.0]),
            bits([0.0, 1.0, 0.0, 0.0]),
            bits([0.0, 0.0, 1.0, 0.0]),
            bits([0.0, 0.0, 0.0, 1.0]),
        ],
        instances: &rows,
        colour_frequency: 3,
        batch_max: 4,
        colour_pool: &pool,
        positions: &positions,
        normals: &normals,
        weights: &weights,
        indices: &indices,
    };
    let max = 1u64 << 24;
    let cpu = deform_group_zero(&inputs, max).expect("CPU deformation");
    let work = gpu_group_zero::prepare(&inputs, max).expect("GPU work");
    let validated = gpu_group_zero::validated(&inputs).expect("validated");
    let directory = root.join(name);
    std::fs::create_dir_all(&directory).unwrap();
    let write = |file: &str, bytes: &[u8]| std::fs::write(directory.join(file), bytes).unwrap();
    write("shader.metal", kernel::MSL.as_bytes());
    write("parameters.bin", &work.parameters);
    write("instances.bin", &work.instances);
    write("vertices.bin", &work.vertices);
    write("pool.bin", &work.pool);
    let mut expected = Vec::new();
    let mut tolerance = Vec::new();
    for (index, (position, colour)) in cpu.positions.iter().zip(&cpu.colours).enumerate() {
        let (instance, vertex) = (index / vertices, index % vertices);
        for word in position.iter().chain(colour) {
            expected.extend_from_slice(&word.to_le_bytes());
        }
        for lane in 0..3 {
            let t = position_tolerance(
                &validated,
                &validated.placements[instance],
                positions[vertex],
                normals[vertex],
                weights[vertex],
                lane,
            );
            tolerance.extend_from_slice(&t.to_le_bytes());
        }
    }
    write("expected.bin", &expected);
    write("tolerance.bin", &tolerance);
    let meta = format!(
        "{{\"entry\": {:?}, \"groups\": {:?}, \"threads\": {:?}, \"vertex_count\": {}}}",
        kernel::MSL_ENTRY,
        work.groups,
        kernel::THREADS,
        work.vertex_count
    );
    write("meta.json", meta.as_bytes());
}

fn main() {
    let root = std::env::args_os().nth(1).expect("output directory");
    let root = Path::new(&root);
    for (name, seed, vertices, instances, phase) in [
        ("small", 1, 7, 5, 0.0),
        ("medium", 2, 33, 170, 12.5),
        ("negative_phase", 3, 64, 90, -3.25),
        ("large_phase", 4, 5, 400, 1.0e4),
        ("many", 5, 1000, 600, 7.75),
    ] {
        write_case(root, name, seed, vertices, instances, phase);
    }
}
