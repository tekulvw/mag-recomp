use mag_render_adapter_scene::{
    material_capture::MaterialCaptureError, material_group_twelve::*,
    partition_inputs::StreamRequest,
};
#[test]
fn raw_pass_twelve_descriptors_are_bounded_owned_sources_not_generated_banks() {
    let mut reads = Vec::new();
    let source = copy_group_twelve_textures(0x1000, true, &mut |r: StreamRequest| {
        reads.push(r);
        Ok(if r.address == 0x11bc {
            0x2000u32.to_be_bytes().to_vec()
        } else if r.address == 0x11c0 {
            0x3000u32.to_be_bytes().to_vec()
        } else {
            vec![r.address as u8; r.bytes]
        })
    })
    .unwrap()
    .unwrap();
    assert_eq!(source.pass_address, 0x1000);
    assert_eq!(source.units[0].address, 0x2000);
    assert_eq!(source.units[1].address, 0x3000);
    assert_eq!(
        reads
            .iter()
            .map(|r| (r.address, r.bytes))
            .collect::<Vec<_>>(),
        [(0x11bc, 4), (0x2000, 32), (0x11c0, 4), (0x3000, 32)]
    );
    assert!(
        copy_group_twelve_textures(0x1000, false, &mut |_| { panic!("disabled source read") })
            .unwrap()
            .is_none()
    );
}
#[test]
fn missing_failed_and_wrapping_group_twelve_sources_never_publish_partial_inputs() {
    let mut reads = 0;
    assert!(
        copy_group_twelve_textures(u32::MAX - 4, true, &mut |_| {
            reads += 1;
            Ok(vec![0; 4])
        })
        .is_err()
    );
    assert_eq!(reads, 0);
    assert!(copy_group_twelve_textures(0x1000, true, &mut |_| Ok(vec![0; 4])).is_err());
    assert!(matches!(
        copy_group_twelve_textures(0x1000, true, &mut |_| Err(MaterialCaptureError::Budget)),
        Err(GroupTwelveError::Source(MaterialCaptureError::Budget))
    ));
}
