// Authored scene contract. Geometry remains GPU-produced; view constants are
// explicit values, with no original packet/register state.
struct View { scale: vec4<f32>, offset: vec4<f32>, }
@group(0) @binding(0) var<uniform> view: View;
struct VertexOut { @builtin(position) position: vec4<f32>, @location(0) uv: vec2<f32>, }
@vertex fn scene_lit_vertex(@location(0) position: vec4<f32>) -> VertexOut {
    var out: VertexOut;
    out.position = vec4<f32>(position.xyz * view.scale.xyz + view.offset.xyz, position.w);
    out.uv = position.xy * 0.5 + vec2<f32>(0.5);
    return out;
}
