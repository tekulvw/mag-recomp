// Authored first scene stage. Its typed storage output becomes a uniform input
// to the following compute stage; no CPU readback/reseed occurs between them.
struct Transform { matrix: mat4x4<f32>, counts: vec4<u32> }
@group(0) @binding(0) var<storage, read> source: array<vec4<f32>>;
@group(0) @binding(1) var<storage, read_write> intermediate: array<vec4<f32>>;
@group(0) @binding(2) var<uniform> translation: Transform;
@group(0) @binding(3) var<uniform> scale_seed: Transform;
@group(0) @binding(4) var<storage, read_write> generated_uniform: Transform;
@compute @workgroup_size(64)
fn scene_transform(@builtin(global_invocation_id) id: vec3<u32>) {
    if any(id.yz != vec2<u32>(0u)) { return; }
    if id.x == 0u { generated_uniform = scale_seed; }
    let count = min(translation.counts.x, min(arrayLength(&source), arrayLength(&intermediate)));
    if id.x < count { intermediate[id.x] = translation.matrix * source[id.x]; }
}
