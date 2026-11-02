// Serial stable compaction correctness reference, not a throughput kernel.
// Keep is nonzero for selected values; selected values retain source order.
struct Candidate {
    value: vec4<f32>,
    keep: u32,
    padding0: u32,
    padding1: u32,
    padding2: u32,
}

struct ResultCount {
    written: u32,
    requested: u32,
    scanned: u32,
    truncated: u32,
}

struct Parameters {
    counts: vec4<u32>,
}

@group(0) @binding(0) var<storage, read> candidates: array<Candidate>;
@group(0) @binding(1) var<storage, read_write> destination: array<vec4<f32>>;
@group(0) @binding(2) var<storage, read_write> result: ResultCount;
@group(0) @binding(3) var<uniform> parameters: Parameters;

@compute @workgroup_size(1)
fn compact(@builtin(global_invocation_id) id: vec3<u32>) {
    if any(id != vec3<u32>(0u)) {
        return;
    }
    let requested = parameters.counts.x;
    let count = min(requested, arrayLength(&candidates));
    let capacity = arrayLength(&destination);
    var written = 0u;
    var truncated = select(0u, 1u, requested > count);
    for (var index = 0u; index < count; index += 1u) {
        if candidates[index].keep != 0u {
            if written < capacity {
                destination[written] = candidates[index].value;
                written += 1u;
            } else {
                truncated = 1u;
            }
        }
    }
    result.written = written;
    result.requested = requested;
    result.scanned = count;
    result.truncated = truncated;
}
