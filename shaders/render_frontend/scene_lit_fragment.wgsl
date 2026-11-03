// Explicit material color, alpha-test, and two separately produced shadow views.
struct Material { tint: vec4<f32>, parameters: vec4<f32>, }
@group(1) @binding(0) var<uniform> material: Material;
@group(1) @binding(1) var albedo: texture_2d<f32>;
@group(1) @binding(2) var color_sampler: sampler;
@group(1) @binding(3) var shadow_a: texture_depth_2d;
@group(1) @binding(4) var shadow_b: texture_depth_2d;
@group(1) @binding(5) var shadow_sampler: sampler_comparison;
@fragment fn scene_lit_fragment(@location(0) uv: vec2<f32>) -> @location(0) vec4<f32> {
    let texel = textureSample(albedo, color_sampler, uv);
    if (texel.a < material.parameters.x) { discard; }
    let a = textureSampleCompare(shadow_a, shadow_sampler, uv, material.parameters.y);
    let b = textureSampleCompare(shadow_b, shadow_sampler, uv, material.parameters.y);
    let light = mix(1.0, (a + b) * 0.5, material.parameters.z);
    return vec4<f32>(texel.rgb * material.tint.rgb * light, texel.a * material.tint.a);
}
