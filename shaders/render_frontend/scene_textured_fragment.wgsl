// Authored textured diagnostic: the sampled colour of one uploaded texture.
@group(1) @binding(1) var albedo: texture_2d<f32>;
@group(1) @binding(2) var color_sampler: sampler;
@fragment fn scene_textured_fragment(@location(0) uv: vec2<f32>) -> @location(0) vec4<f32> {
    return textureSample(albedo, color_sampler, uv);
}
