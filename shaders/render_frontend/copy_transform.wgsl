// Synthetic float4 transform. Runtime lengths bound every storage access.
// Transform is column-major; the host allocates at least one element per array,
// even for count zero. counts.x is the requested logical item count.
struct Transform {
    matrix: mat4x4<f32>,
    counts: vec4<u32>,
}

@group(0) @binding(0) var<storage, read> source: array<vec4<f32>>;
@group(0) @binding(1) var<storage, read_write> destination: array<vec4<f32>>;
@group(0) @binding(2) var<uniform> parameters: Transform;

@compute @workgroup_size(64)
fn copy_transform(@builtin(global_invocation_id) id: vec3<u32>) {
    if any(id.yz != vec2<u32>(0u)) {
        return;
    }
    let count = min(parameters.counts.x, min(arrayLength(&source), arrayLength(&destination)));
    if id.x < count {
        destination[id.x] = parameters.matrix * source[id.x];
    }
}
