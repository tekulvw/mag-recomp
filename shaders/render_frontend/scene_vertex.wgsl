// Authored portable scene: GPU compute output is the actual vertex buffer.
@vertex
fn scene_vertex(@location(0) position: vec4<f32>) -> @builtin(position) vec4<f32> {
    return position;
}
