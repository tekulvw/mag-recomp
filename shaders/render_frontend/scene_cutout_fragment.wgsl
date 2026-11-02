// Depth-only shadow cutout. No color output; discarded texels must leave the
// explicitly cleared/loaded depth untouched.
@group(1) @binding(1) var cutout: texture_2d<f32>;
@group(1) @binding(2) var cutout_sampler: sampler;
@fragment fn scene_cutout_fragment(@location(0) uv: vec2<f32>) {
    if (textureSample(cutout,cutout_sampler,uv).a < 0.5) { discard; }
}
