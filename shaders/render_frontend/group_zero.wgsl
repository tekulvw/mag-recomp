// Group-zero (instanced wind) deformation, one invocation per expanded vertex.
// Written from docs/render-frontend/behavior/group-zero-deformation.md. The CPU reference is
// mag-render-behavior's group_zero::deform_vertex; the host validates every refusal and computes
// the per-instance placements before dispatch, so this kernel has no refusal of its own.
// Output: two rows per vertex, instance-major: the deformed position (x, y, z, 1), then the
// slot-12 colour row.

struct Parameters {
    // Source vertices V, instances I, colour frequency F, batch maximum B.
    counts: vec4<u32>,
    // Invocations per dispatch row (the x extent of the grid), then unused.
    grid: vec4<u32>,
    // Wave frequency f, amplitudes A1 and A2, phase theta.
    wave: vec4<f32>,
    // Tuning coefficients KA1, KB1 and KC, then unused.
    coefficients: vec4<f32>,
    // byte / 255 for every byte value, four per row.
    colours: array<vec4<f32>, 64>,
}

// Rows of scale * rotation, with the instance offset in w.
struct Instance {
    row0: vec4<f32>,
    row1: vec4<f32>,
    row2: vec4<f32>,
}

// Source position (x, y, z, w3) and normal (x, y, z, unused).
struct SourceVertex {
    position: vec4<f32>,
    normal: vec4<f32>,
}

@group(0) @binding(0) var<uniform> parameters: Parameters;
@group(0) @binding(1) var<storage, read> instances: array<Instance>;
@group(0) @binding(2) var<storage, read> source: array<SourceVertex>;
@group(0) @binding(3) var<storage, read> pool: array<u32>;
@group(0) @binding(4) var<storage, read_write> output: array<vec4<f32>>;

fn fraction(x: f32) -> f32 {
    let f = x - floor(x);
    return select(f, 0.0, f >= 1.0);
}

fn triangle(x: f32) -> f32 {
    return abs(2.0 * fraction(x) - 1.0);
}

fn smoothed(t: f32) -> f32 {
    return t * t * (3.0 - 2.0 * t);
}

fn wave(phase: f32, frequency: f32) -> f32 {
    return smoothed(triangle(frequency * (2.0 * fraction(phase) - 1.0) + 0.5));
}

fn channel(packed: u32, shift: u32) -> f32 {
    let byte = (packed >> shift) & 255u;
    return parameters.colours[byte / 4u][byte % 4u];
}

@compute @workgroup_size(64)
fn group_zero(@builtin(global_invocation_id) id: vec3<u32>) {
    let vertices = parameters.counts.x;
    let total = vertices * parameters.counts.y;
    let index = id.x + id.y * parameters.grid.x;
    if id.z != 0u || id.x >= parameters.grid.x || index >= total {
        return;
    }
    let instance = index / vertices;
    let vertex = index - instance * vertices;
    let placement = instances[instance];
    let s = source[vertex];
    let p = s.position.xyz;
    let placed = vec3<f32>(
        dot(placement.row0.xyz, p) + placement.row0.w,
        dot(placement.row1.xyz, p) + placement.row1.w,
        dot(placement.row2.xyz, p) + placement.row2.w,
    );
    let theta = parameters.wave.w;
    let alpha = parameters.coefficients.x * (theta + placed.x + placed.y);
    let beta = parameters.coefficients.y * (theta + parameters.coefficients.z * (placed.x + placed.y + placed.z));
    let frequency = parameters.wave.x;
    let displacement = (parameters.wave.y * wave(beta, frequency) + parameters.wave.z * wave(alpha, frequency)) * (1.0 - s.position.w);
    output[2u * index] = vec4<f32>(
        placed.x + displacement * s.normal.x,
        placed.y + displacement * s.normal.y,
        placed.z + displacement * s.normal.x,
        1.0,
    );
    let batch = parameters.counts.w;
    let first = (instance / batch) * batch;
    let entry = first + ((instance - first) * vertices + vertex) / parameters.counts.z;
    let packed = pool[entry];
    output[2u * index + 1u] = vec4<f32>(channel(packed, 0u), channel(packed, 8u), channel(packed, 16u), channel(packed, 24u));
}
