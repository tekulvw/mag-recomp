// Authored opaque diagnostic color. No textures, game formats, or host readback.
@fragment
fn scene_fragment() -> @location(0) vec4<f32> {
    return vec4<f32>(0.2, 0.6, 1.0, 1.0);
}
