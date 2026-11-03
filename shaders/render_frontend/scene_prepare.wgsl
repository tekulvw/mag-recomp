// Authored synthetic geometry preparation. Reads a prior GPU-produced value;
// vertices and uint32 triangle indices are consumed directly by the draw.
struct Transform { matrix: mat4x4<f32>, counts: vec4<u32> }
@group(0) @binding(0) var<storage, read> source: array<vec4<f32>>;
@group(0) @binding(1) var<storage, read_write> vertices: array<vec4<f32>>;
@group(0) @binding(2) var<storage, read_write> indices: array<u32>;
@group(0) @binding(3) var<uniform> parameters: Transform;
@compute @workgroup_size(64)
fn scene_prepare(@builtin(global_invocation_id) id: vec3<u32>) {
    if any(id.yz != vec2<u32>(0u)) { return; }
    let count = min(parameters.counts.x, min(arrayLength(&source), min(arrayLength(&vertices), arrayLength(&indices))));
    if id.x < count {
        vertices[id.x] = parameters.matrix * source[id.x];
        indices[id.x] = id.x;
    }
}
