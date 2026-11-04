use mag_render_adapter_scene::cohort_geometry::direct::*;
fn word(b: &mut [u8], at: usize, v: u32) {
    b[at..at + 4].copy_from_slice(&v.to_be_bytes())
}
fn half(b: &mut [u8], at: usize, v: u16) {
    b[at..at + 2].copy_from_slice(&v.to_be_bytes())
}
fn source(vertices: u32, indices: u32) -> ([u8; 192], [u8; 656], [u8; 112]) {
    let mut mesh = [0; 192];
    let mut layout = [0; 656];
    for i in 0..16 {
        word(&mut layout, 0x110 + i * 20, i as u32)
    }
    for (at, v) in [
        (0x34, vertices),
        (0x38, indices),
        (0x3c, 0x3000),
        (0x44, 5),
        (0x48, 0x10),
        (0x4c, 0x1000),
        (0x5c, 0x2000),
    ] {
        word(&mut mesh, at, v)
    }
    word(&mut layout, 0, 0x00041740);
    word(&mut layout, 4, 0x0c32);
    word(&mut layout, 0x80, 0x00041fc0);
    word(&mut layout, 0x110 + 12, 2);
    word(&mut layout, 0x110 + 16, 3);
    half(&mut layout, 0x250, 12);
    (mesh, layout, [0; 112])
}
fn vertices(n: usize) -> Vec<u8> {
    (0..n)
        .flat_map(|i| {
            [i as f32, 2., 3.]
                .into_iter()
                .flat_map(|f| f.to_bits().to_be_bytes())
        })
        .collect()
}
#[test]
fn native_u32_source_indices_are_not_narrowed_to_sixteen_bits() {
    let (mut mesh, layout, t) = source(65538, 3);
    word(&mut mesh, 0x48, 0);
    let positions = vertices(65538);
    let index = [65537u32, 0, 1]
        .into_iter()
        .flat_map(u32::to_be_bytes)
        .collect::<Vec<_>>();
    let d = copy_direct_draw(
        &mesh,
        &layout,
        &t,
        false,
        1 << 30,
        4 * 1024 * 1024,
        &mut |r| {
            Ok(if r.offset == 0x1000 {
                positions.clone()
            } else {
                index.clone()
            })
        },
    )
    .unwrap();
    assert_eq!(d.vertex_count, 65538);
    assert_eq!(d.indices, [65537, 0, 1]);
}
#[test]
fn array_strip_preserves_alternating_winding_and_all_source_vertices() {
    let (mut mesh, layout, t) = source(4, 0);
    word(&mut mesh, 0x44, 6);
    let positions = vertices(4);
    let d = copy_direct_draw(&mesh, &layout, &t, false, 1 << 30, 4096, &mut |_| {
        Ok(positions.clone())
    })
    .unwrap();
    assert_eq!(d.indices, [0, 1, 2, 2, 1, 3]);
}
#[test]
fn invalid_source_states_and_native_budget_fail_with_concrete_context() {
    let (mut mesh, layout, t) = source(3, 3);
    assert_eq!(
        copy_direct_draw(&mesh, &layout, &t, false, 1 << 30, 1, &mut |_| panic!(
            "budget must refuse before read"
        ))
        .err(),
        Some(DirectDrawError::Budget)
    );
    word(&mut mesh, 0x4c, 0);
    let indices = [0u16, 1, 2]
        .into_iter()
        .flat_map(u16::to_be_bytes)
        .collect::<Vec<_>>();
    assert_eq!(
        copy_direct_draw(&mesh, &layout, &t, false, 1 << 30, 4096, &mut |_| Ok(
            indices.clone()
        ))
        .err(),
        Some(DirectDrawError::MissingStream { slot: 0, stream: 0 })
    );
}
