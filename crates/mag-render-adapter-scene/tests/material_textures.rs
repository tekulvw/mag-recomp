use mag_render_adapter_scene::material_textures::{
    NativeTextureFormat, TextureError, TextureLocation, decode_texture_image, texture_layout,
};

fn descriptor(format: u8, width: u32, height: u32, levels: u32, cube: bool) -> [u32; 8] {
    [
        0x120,
        2 | (u32::from(cube) << 2) | 0x20 | (u32::from(format) << 8) | (levels << 16),
        0,
        0x80000000,
        0xaae4,
        0,
        (width << 16) | height,
        0,
    ]
}

#[test]
fn actual_argb_pixels_unswizzle_and_all_mips_survive() {
    let d = descriptor(0x85, 4, 2, 3, false);
    let layout = texture_layout(d, 1024, 1024).unwrap();
    assert_eq!(layout.location(), TextureLocation::Main);
    assert_eq!(layout.source_bytes(), 44);
    let mut source = vec![0; 44];
    // 4x2 rectangular Morton order: x0y0,x1y0,x0y1,x1y1,x2y0,...
    for (ordinal, value) in [10, 11, 20, 21, 12, 13, 22, 23, 30, 31, 40]
        .into_iter()
        .enumerate()
    {
        source[ordinal * 4..ordinal * 4 + 4].copy_from_slice(&[255, value, 0, 0]);
    }
    let image = decode_texture_image(layout, source).unwrap();
    assert_eq!(image.format, NativeTextureFormat::Rgba8Unorm);
    assert_eq!(image.levels.len(), 3);
    assert_eq!(
        image.levels[0]
            .bytes
            .as_chunks::<4>()
            .0
            .iter()
            .map(|p| p[0])
            .collect::<Vec<_>>(),
        [10, 11, 12, 13, 20, 21, 22, 23]
    );
    assert_eq!(image.levels[1].bytes, [30, 0, 0, 255, 31, 0, 0, 255]);
    assert_eq!(image.levels[2].bytes, [40, 0, 0, 255]);
    assert_eq!(image.source_bytes.len(), 44);
}

#[test]
fn linear_pitch_padding_is_not_uploaded_as_pixels() {
    let mut d = descriptor(0xa5, 2, 2, 1, false);
    d[7] = 12;
    let layout = texture_layout(d, 24, 16).unwrap();
    let image = decode_texture_image(
        layout,
        vec![
            128, 1, 2, 3, 255, 4, 5, 6, 99, 99, 99, 99, 64, 7, 8, 9, 0, 10, 11, 12, 88, 88, 88, 88,
        ],
    )
    .unwrap();
    assert_eq!(
        image.levels[0].bytes,
        [1, 2, 3, 128, 4, 5, 6, 255, 7, 8, 9, 64, 10, 11, 12, 0]
    );
}

#[test]
fn compressed_cube_faces_keep_source_blocks_and_aligned_mip_pyramids() {
    let d = descriptor(0x88, 4, 4, 3, true);
    let layout = texture_layout(d, 768, 288).unwrap();
    assert_eq!(layout.source_bytes(), 688); // last face's48 bytes, no trailing padding read
    assert_eq!(layout.levels().len(), 18);
    let mut source = vec![0; layout.source_bytes()];
    for face in 0..6 {
        source[face * 128..face * 128 + 48].fill(face as u8 + 1);
    }
    let image = decode_texture_image(layout, source).unwrap();
    assert_eq!(image.format, NativeTextureFormat::Bc3RgbaUnorm);
    assert_eq!(image.levels[15].face, 5);
    assert_eq!(image.levels[15].bytes, vec![6; 16]);
    assert_eq!(image.levels[17].bytes, vec![6; 16]);
}

#[test]
fn invalid_format_location_or_extent_never_requests_an_image() {
    let mut d = descriptor(0x85, 2, 2, 1, false);
    assert_eq!(texture_layout(d, 15, 16).unwrap_err(), TextureError::Budget);
    d[1] &= !3;
    assert_eq!(
        texture_layout(d, 1024, 1024).unwrap_err(),
        TextureError::Location(0)
    );
    d = descriptor(0x8d, 2, 2, 1, false);
    assert_eq!(
        texture_layout(d, 1024, 1024).unwrap_err(),
        TextureError::Format(0x8d)
    );
    d = descriptor(0x85, 2, 2, 1, false);
    let layout = texture_layout(d, 16, 16).unwrap();
    assert_eq!(
        decode_texture_image(layout, vec![0; 15]).unwrap_err(),
        TextureError::Extent
    );
}

#[test]
fn primary_noop_texture_records_do_not_read_descriptors_or_replace_bindings() {
    use mag_render_adapter_scene::material_textures::*;
    fn source(index: usize, route: u8, object: u32) -> TextureObjectSource {
        let mut record = [0; 32];
        record[4..8].copy_from_slice(&object.to_be_bytes());
        record[12] = route;
        record[13] = 255; // Never consumed on a no-op or resident path.
        TextureObjectSource {
            record_index: index,
            record,
            target_word: None,
            status: None,
            descriptor: None,
        }
    }
    let mut sources = vec![
        source(2, 0, 0x1000),
        source(4, 1, 0x2000),
        source(8, 2, 0x3000),
        source(12, 255, 0),
    ];
    let raw = descriptor(0xa5, 2, 2, 1, false);
    let mut resident = source(16, 255, 0x4000);
    resident.target_word = Some(8 << 14);
    resident.status = Some([2; 16]);
    resident.descriptor = Some(std::array::from_fn(|i| raw[i / 4].to_be_bytes()[i % 4]));
    sources.push(resident);
    // A real non-no-op missing descriptor must retain its typed refusal.
    sources.push(source(20, 3, 0x5000));
    let mut budget = 64;
    let mut reads = 0;
    let output = copy_primary_texture_bindings(
        &sources,
        &std::array::from_fn(|_| None),
        &mut budget,
        &mut TextureImageCache::default(),
        &mut |request| {
            reads += 1;
            assert_eq!(request.offset, 0x120);
            assert_eq!(request.bytes, 16);
            Ok(vec![0; 16])
        },
    )
    .unwrap();
    assert_eq!(output.len(), 2);
    assert_eq!(texture_binding_record_index(&sources, 0), Some(16));
    assert_eq!(texture_binding_record_index(&sources, 1), Some(20));
    assert_eq!(texture_binding_record_index(&sources, 2), None);
    let applied = output[0].as_ref().unwrap();
    assert_eq!(applied.record_index, 16);
    assert_eq!(applied.unit, 8);
    assert_eq!(applied.descriptor, raw);
    assert!(applied.image.cpu().unwrap().is_ok());
    assert_eq!(
        output[1].as_ref().unwrap_err(),
        &TextureError::MissingSource
    );
    assert_eq!(reads, 1);
    assert_eq!(sources.len(), 6); // Raw requests remain available for diagnostics.
}
