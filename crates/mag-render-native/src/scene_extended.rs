//! Synchronous owned tables for additive native scene-V3. No target-derived
//! source viewport or image format/dimension fallback is introduced.
use super::*;
use scene::{ImageLayout, Subresource, ViewDesc, Viewport};
#[repr(C)]
#[derive(Clone, Copy)]
struct Layout {
    dimension: u32,
    depth: u32,
    layers: u32,
    mip_count: u32,
    samples: u32,
    reserved: u32,
    subresources: *const SubresourceWire,
    subresource_count: u32,
    reserved1: u32,
}
#[repr(C)]
#[derive(Clone, Copy)]
struct SubresourceWire {
    mip: u32,
    layer: u32,
    width: u32,
    height: u32,
    depth: u32,
    row_bytes: u32,
    offset: u64,
    length: u64,
}
#[repr(C)]
#[derive(Clone, Copy)]
struct View {
    image: u32,
    dimension: u32,
    base_mip: u32,
    mip_count: u32,
    base_layer: u32,
    layer_count: u32,
    remap: [u32; 4],
    reserved: u32,
}
#[repr(C)]
struct SceneV3 {
    struct_size: u32,
    abi_version: u32,
    base: scene_sampler::SceneV2,
    image_layouts: *const Layout,
    image_layout_count: u32,
    view_count: u32,
    views: *const View,
    viewports: *const Viewport,
    viewport_count: u32,
    reserved: u32,
}
#[repr(C)]
#[derive(Clone, Copy)]
struct StencilWire {
    image: u32,
    load: u32,
    store: u32,
    clear_value: u32,
    flags: u32,
    reserved: u32,
}
#[repr(C)]
struct SceneV4 {
    struct_size: u32,
    abi_version: u32,
    base: SceneV3,
    stencils: *const StencilWire,
    stencil_count: u32,
    reserved: u32,
}
pub(super) struct Import {
    pub image: u32,
    pub required_initialized_planes: u32,
    pub handle: crate::host_image::ImageHandle,
}
#[repr(C)]
#[derive(Clone, Copy)]
struct ImportWire {
    image: u32,
    required_initialized_planes: u32,
    reference: crate::host_image::NativeImage,
}
#[repr(C)]
#[derive(Clone, Copy)]
struct BoundsWire {
    enabled: u32,
    reserved: u32,
    minimum: f32,
    maximum: f32,
}
#[repr(C)]
struct SceneV5 {
    struct_size: u32,
    abi_version: u32,
    base: SceneV4,
    imports: *const ImportWire,
    import_count: u32,
    reserved: u32,
    bounds: *const BoundsWire,
    bounds_count: u32,
    reserved1: u32,
}
unsafe extern "C" {
    fn mag_native_reserve_scene_v5(
        device: *mut c_void,
        imports: *const ImportWire,
        count: u32,
        out: *mut Ticket,
    ) -> i32;
    fn mag_native_abandon_scene_reservation(device: *mut c_void) -> i32;
    fn mag_native_scene_chunk_stats(device: *mut c_void, out: *mut u64, count: u32) -> i32;
    fn mag_native_memory_stats(device: *mut c_void, out: *mut u64, count: u32) -> i32;
    fn mag_native_scene_v5_layout(out: *mut u32, words: u32) -> i32;
    fn mag_native_submit_scene_v5(
        device: *mut c_void,
        scene: *const SceneV5,
        target: *const crate::host_image::NativeImage,
        frame: *const crate::host_frame::HostFrame,
        out: *mut Ticket,
    ) -> i32;
    fn mag_native_submit_scene_v4(
        device: *mut c_void,
        scene: *const SceneV4,
        target: *const crate::host_image::NativeImage,
        frame: *const crate::host_frame::HostFrame,
        out: *mut Ticket,
    ) -> i32;
    fn mag_native_scene_v4_layout(out: *mut u32, words: u32) -> i32;

    fn mag_native_submit_scene_v3(
        device: *mut c_void,
        scene: *const SceneV3,
        target: *const crate::host_image::NativeImage,
        frame: *const crate::host_frame::HostFrame,
        out: *mut Ticket,
    ) -> i32;
    fn mag_native_scene_v3_layout(out: *mut u32, words: u32) -> i32;
    fn mag_native_image_format_layout(format: u32, out: *mut u32, words: u32) -> i32;
}
fn sub(s: Subresource) -> SubresourceWire {
    SubresourceWire {
        mip: s.mip,
        layer: s.layer,
        width: s.width,
        height: s.height,
        depth: s.depth,
        row_bytes: s.row_bytes,
        offset: s.offset,
        length: s.length,
    }
}
pub(super) fn checked_layout() -> Result<(), i32> {
    let expected = [
        size_of::<SubresourceWire>() as u32,
        align_of::<SubresourceWire>() as u32,
        offset_of!(SubresourceWire, mip) as u32,
        offset_of!(SubresourceWire, layer) as u32,
        offset_of!(SubresourceWire, width) as u32,
        offset_of!(SubresourceWire, height) as u32,
        offset_of!(SubresourceWire, depth) as u32,
        offset_of!(SubresourceWire, row_bytes) as u32,
        offset_of!(SubresourceWire, offset) as u32,
        offset_of!(SubresourceWire, length) as u32,
        size_of::<Layout>() as u32,
        align_of::<Layout>() as u32,
        offset_of!(Layout, dimension) as u32,
        offset_of!(Layout, depth) as u32,
        offset_of!(Layout, layers) as u32,
        offset_of!(Layout, mip_count) as u32,
        offset_of!(Layout, samples) as u32,
        offset_of!(Layout, reserved) as u32,
        offset_of!(Layout, subresources) as u32,
        offset_of!(Layout, subresource_count) as u32,
        offset_of!(Layout, reserved1) as u32,
        size_of::<View>() as u32,
        align_of::<View>() as u32,
        offset_of!(View, image) as u32,
        offset_of!(View, dimension) as u32,
        offset_of!(View, base_mip) as u32,
        offset_of!(View, mip_count) as u32,
        offset_of!(View, base_layer) as u32,
        offset_of!(View, layer_count) as u32,
        offset_of!(View, remap) as u32,
        offset_of!(View, reserved) as u32,
        size_of::<Viewport>() as u32,
        align_of::<Viewport>() as u32,
        offset_of!(Viewport, x) as u32,
        offset_of!(Viewport, y) as u32,
        offset_of!(Viewport, width) as u32,
        offset_of!(Viewport, height) as u32,
        offset_of!(Viewport, min_depth) as u32,
        offset_of!(Viewport, max_depth) as u32,
        offset_of!(Viewport, scissor_x) as u32,
        offset_of!(Viewport, scissor_y) as u32,
        offset_of!(Viewport, scissor_width) as u32,
        offset_of!(Viewport, scissor_height) as u32,
        offset_of!(Viewport, known_fields) as u32,
        offset_of!(Viewport, reserved) as u32,
        size_of::<SceneV3>() as u32,
        align_of::<SceneV3>() as u32,
        offset_of!(SceneV3, struct_size) as u32,
        offset_of!(SceneV3, abi_version) as u32,
        offset_of!(SceneV3, base) as u32,
        offset_of!(SceneV3, image_layouts) as u32,
        offset_of!(SceneV3, image_layout_count) as u32,
        offset_of!(SceneV3, view_count) as u32,
        offset_of!(SceneV3, views) as u32,
        offset_of!(SceneV3, viewports) as u32,
        offset_of!(SceneV3, viewport_count) as u32,
        offset_of!(SceneV3, reserved) as u32,
    ];
    let mut actual = [0; 57];
    let status = unsafe { mag_native_scene_v3_layout(actual.as_mut_ptr(), actual.len() as u32) };
    if status != 0 {
        return Err(status);
    }
    if actual != expected {
        return Err(-1);
    }
    for id in 1..=16 {
        let f = scene::ImageFormat::from_native(id).map_err(|_| -1)?.info();
        let expected = [
            f.block_width,
            f.block_height,
            f.block_bytes,
            f.sample_kind,
            u32::from(f.color_attachment),
            u32::from(f.depth_attachment),
        ];
        let mut actual = [0; 6];
        let status = unsafe { mag_native_image_format_layout(id, actual.as_mut_ptr(), 6) };
        if status != 0 {
            return Err(status);
        }
        if actual != expected {
            return Err(-1);
        }
    }
    let expected = [
        size_of::<StencilWire>() as u32,
        align_of::<StencilWire>() as u32,
        offset_of!(StencilWire, image) as u32,
        offset_of!(StencilWire, load) as u32,
        offset_of!(StencilWire, store) as u32,
        offset_of!(StencilWire, clear_value) as u32,
        offset_of!(StencilWire, flags) as u32,
        offset_of!(StencilWire, reserved) as u32,
        size_of::<SceneV4>() as u32,
        align_of::<SceneV4>() as u32,
        offset_of!(SceneV4, struct_size) as u32,
        offset_of!(SceneV4, abi_version) as u32,
        offset_of!(SceneV4, base) as u32,
        offset_of!(SceneV4, stencils) as u32,
        offset_of!(SceneV4, stencil_count) as u32,
        offset_of!(SceneV4, reserved) as u32,
    ];
    let mut actual = [0; 16];
    let status = unsafe { mag_native_scene_v4_layout(actual.as_mut_ptr(), 16) };
    if status != 0 {
        return Err(status);
    }
    if actual != expected {
        return Err(-1);
    }
    Ok(())
}
pub(super) struct Plan {
    _subresources: Vec<Vec<SubresourceWire>>,
    layouts: Vec<Layout>,
    views: Vec<View>,
    viewports: Vec<Viewport>,
    stencils: Option<Vec<StencilWire>>,
    bounds: Vec<BoundsWire>,
    pub(super) texture_codes: Vec<Vec<u32>>,
}
impl Plan {
    pub(super) fn build(commands: &scene::SceneCommands) -> Result<Option<Self>, i32> {
        let v4 = commands.stencils().iter().any(Option::is_some)
            || commands.passes().iter().any(|p| {
                p.color
                    .is_some_and(|a| matches!(a.load, scene::Load::Discard))
                    || p.depth
                        .is_some_and(|a| matches!(a.load, scene::Load::Discard))
            });
        let extended = commands
            .depth_bounds()
            .iter()
            .flatten()
            .any(Option::is_some)
            || commands.images().iter().any(|i| i.external.is_some())
            || v4
            || commands
                .images()
                .iter()
                .any(|i| i.layout.is_some() || i.desc.format.native_id() > 3)
            || commands
                .passes()
                .iter()
                .flat_map(|p| &p.draws)
                .flat_map(|d| &d.textures)
                .any(|t| t.image.view().is_some())
            || commands.viewports().iter().any(Option::is_some);
        if !extended {
            return Ok(None);
        }
        checked_layout()?;
        checked_v5_layout()?;
        let mut subresources = Vec::new();
        let mut layouts = Vec::new();
        for image in commands.images() {
            let shape = image.layout.clone().unwrap_or_else(|| {
                let mut layout = ImageLayout::default();
                if image.upload.is_some() {
                    let level = image
                        .desc
                        .format
                        .level_layout(image.desc.width, image.desc.height)
                        .expect("prepared format/extent");
                    layout.subresources.push(Subresource {
                        mip: 0,
                        layer: 0,
                        width: image.desc.width,
                        height: image.desc.height,
                        depth: 1,
                        row_bytes: level.row_bytes as u32,
                        offset: 0,
                        length: level.bytes,
                    });
                }
                layout
            });
            subresources.push(
                shape
                    .subresources
                    .iter()
                    .copied()
                    .map(sub)
                    .collect::<Vec<_>>(),
            );
            let resources = subresources.last().unwrap();
            layouts.push(Layout {
                dimension: shape.dimension.native_id(),
                depth: shape.depth,
                layers: shape.layers,
                mip_count: shape.mip_count,
                samples: shape.samples,
                reserved: 0,
                subresources: resources.as_ptr(),
                subresource_count: resources.len() as u32,
                reserved1: 0,
            });
        }
        let mut keys: Vec<(u32, ViewDesc)> = Vec::new();
        let mut views = Vec::new();
        let mut codes = Vec::new();
        for draw in commands.passes().iter().flat_map(|p| &p.draws) {
            let mut row = Vec::new();
            for t in &draw.textures {
                let image = t.image.image().index() as u32;
                if let Some(v) = t.image.view() {
                    let k = (image, v);
                    let index = if let Some(i) = keys.iter().position(|old| *old == k) {
                        i
                    } else {
                        if keys.len() >= 4096 {
                            return Err(-5);
                        }
                        let i = keys.len();
                        keys.push(k);
                        views.push(View {
                            image,
                            dimension: v.dimension.native_id(),
                            base_mip: v.base_mip,
                            mip_count: v.mip_count,
                            base_layer: v.base_layer,
                            layer_count: v.layer_count,
                            remap: v.remap,
                            reserved: 0,
                        });
                        i
                    };
                    row.push(0x80000000u32 | index as u32)
                } else {
                    row.push(image)
                }
            }
            codes.push(row)
        }
        let viewports = commands
            .viewports()
            .iter()
            .flatten()
            .copied()
            .collect::<Vec<_>>();
        if !viewports.is_empty() && viewports.len() != commands.passes().len() {
            return Err(-1);
        }
        Ok(Some(Self {
            bounds: if commands
                .depth_bounds()
                .iter()
                .flatten()
                .any(Option::is_some)
            {
                commands
                    .depth_bounds()
                    .iter()
                    .flatten()
                    .map(|b| BoundsWire {
                        enabled: u32::from(b.is_some()),
                        reserved: 0,
                        minimum: b.map_or(0., |b| b.minimum),
                        maximum: b.map_or(0., |b| b.maximum),
                    })
                    .collect()
            } else {
                Vec::new()
            },
            _subresources: subresources,
            layouts,
            views,
            viewports,
            stencils: if v4 {
                Some(
                    commands
                        .stencils()
                        .iter()
                        .map(|s| match s {
                            Some(s) => {
                                let (load, clear) = match s.load {
                                    scene::StencilLoad::Clear(v) => (0, u32::from(v)),
                                    scene::StencilLoad::Load => (1, 0),
                                    scene::StencilLoad::Discard => (2, 0),
                                };
                                StencilWire {
                                    image: s.image.index() as u32,
                                    load,
                                    store: u32::from(s.store == scene::Store::Discard),
                                    clear_value: clear,
                                    flags: u32::from(s.source_full_write),
                                    reserved: 0,
                                }
                            }
                            None => StencilWire {
                                image: u32::MAX,
                                load: 0,
                                store: 0,
                                clear_value: 0,
                                flags: 0,
                                reserved: 0,
                            },
                        })
                        .collect(),
                )
            } else {
                None
            },
            texture_codes: codes,
        }))
    }
    /// Reserve the next imported submission's native queue position/ticket
    /// and validate exactly these imports under the owner lease.
    /// # Safety
    /// `device` is the live native device owning every import handle.
    pub(super) unsafe fn reserve(device: *mut c_void, imports: &[Import], out: &mut Ticket) -> i32 {
        let rows = imports
            .iter()
            .map(|i| ImportWire {
                image: i.image,
                required_initialized_planes: i.required_initialized_planes,
                reference: i.handle.raw,
            })
            .collect::<Vec<_>>();
        unsafe { mag_native_reserve_scene_v5(device, rows.as_ptr(), rows.len() as u32, out) }
    }
    /// Reserve only the next imported submission's queue position and ticket;
    /// that submission validates its own imports under a short owner lease
    /// when it is encoded (backends that cannot report UNSUPPORTED).
    /// # Safety
    /// `device` is the live native device.
    pub(super) unsafe fn reserve_position(device: *mut c_void, out: &mut Ticket) -> i32 {
        unsafe { mag_native_reserve_scene_v5(device, std::ptr::null(), 0, out) }
    }
    /// # Safety
    /// `device` is the live native device.
    pub(super) unsafe fn abandon_reservation(device: *mut c_void) -> i32 {
        unsafe { mag_native_abandon_scene_reservation(device) }
    }
    /// # Safety
    /// `device` is the live native device.
    pub(super) unsafe fn chunk_stats(device: *mut c_void, out: &mut [u64; 3]) -> i32 {
        unsafe { mag_native_scene_chunk_stats(device, out.as_mut_ptr(), 3) }
    }
    /// # Safety
    /// `device` is the live native device.
    pub(super) unsafe fn memory_stats(device: *mut c_void, out: &mut [u64; 9]) -> i32 {
        unsafe { mag_native_memory_stats(device, out.as_mut_ptr(), 9) }
    }
    pub(super) unsafe fn submit(
        &self,
        device: *mut c_void,
        scene: &Scene,
        samplers: &[scene_sampler::Handle],
        imports: &[Import],
        output: (
            Option<&crate::host_image::NativeImage>,
            Option<&crate::host_frame::HostFrame>,
        ),
        out: &mut Ticket,
    ) -> i32 {
        let (target, frame) = output;
        let v3 = SceneV3 {
            struct_size: size_of::<SceneV3>() as u32,
            abi_version: 3,
            base: scene_sampler::SceneV2 {
                struct_size: size_of::<scene_sampler::SceneV2>() as u32,
                abi_version: 2,
                scene,
                samplers: samplers.as_ptr(),
                sampler_count: samplers.len() as u32,
                reserved: 0,
            },
            image_layouts: self.layouts.as_ptr(),
            image_layout_count: self.layouts.len() as u32,
            view_count: self.views.len() as u32,
            views: self.views.as_ptr(),
            viewports: self.viewports.as_ptr(),
            viewport_count: self.viewports.len() as u32,
            reserved: 0,
        };
        if !imports.is_empty() || !self.bounds.is_empty() {
            let absent = vec![
                StencilWire {
                    image: u32::MAX,
                    load: 0,
                    store: 0,
                    clear_value: 0,
                    flags: 0,
                    reserved: 0
                };
                scene.pass_count as usize
            ];
            let stencils = self.stencils.as_ref().unwrap_or(&absent);
            let rows = imports
                .iter()
                .map(|i| ImportWire {
                    image: i.image,
                    required_initialized_planes: i.required_initialized_planes,
                    reference: i.handle.raw,
                })
                .collect::<Vec<_>>();
            let desc = SceneV5 {
                struct_size: size_of::<SceneV5>() as u32,
                abi_version: 5,
                base: SceneV4 {
                    struct_size: size_of::<SceneV4>() as u32,
                    abi_version: 4,
                    base: v3,
                    stencils: stencils.as_ptr(),
                    stencil_count: stencils.len() as u32,
                    reserved: 0,
                },
                imports: rows.as_ptr(),
                import_count: rows.len() as u32,
                reserved: 0,
                bounds: self.bounds.as_ptr(),
                bounds_count: self.bounds.len() as u32,
                reserved1: 0,
            };
            return unsafe {
                mag_native_submit_scene_v5(
                    device,
                    &desc,
                    target.map_or(std::ptr::null(), |t| t),
                    frame.map_or(std::ptr::null(), |f| f),
                    out,
                )
            };
        }
        if let Some(stencils) = &self.stencils {
            let v4 = SceneV4 {
                struct_size: size_of::<SceneV4>() as u32,
                abi_version: 4,
                base: v3,
                stencils: stencils.as_ptr(),
                stencil_count: stencils.len() as u32,
                reserved: 0,
            };
            return unsafe {
                mag_native_submit_scene_v4(
                    device,
                    &v4,
                    target.map_or(std::ptr::null(), |t| t),
                    frame.map_or(std::ptr::null(), |f| f),
                    out,
                )
            };
        }
        unsafe {
            mag_native_submit_scene_v3(
                device,
                &v3,
                target.map_or(std::ptr::null(), |t| t),
                frame.map_or(std::ptr::null(), |f| f),
                out,
            )
        }
    }
}

fn checked_v5_layout() -> Result<(), i32> {
    use crate::host_image::Certificate;
    let expected = [
        size_of::<Certificate>() as u32,
        align_of::<Certificate>() as u32,
        offset_of!(Certificate, host) as u32,
        offset_of!(Certificate, validate_planes) as u32,
        size_of::<ImportWire>() as u32,
        align_of::<ImportWire>() as u32,
        offset_of!(ImportWire, image) as u32,
        offset_of!(ImportWire, required_initialized_planes) as u32,
        offset_of!(ImportWire, reference) as u32,
        size_of::<BoundsWire>() as u32,
        align_of::<BoundsWire>() as u32,
        offset_of!(BoundsWire, enabled) as u32,
        offset_of!(BoundsWire, reserved) as u32,
        offset_of!(BoundsWire, minimum) as u32,
        offset_of!(BoundsWire, maximum) as u32,
        size_of::<SceneV5>() as u32,
        align_of::<SceneV5>() as u32,
        offset_of!(SceneV5, struct_size) as u32,
        offset_of!(SceneV5, abi_version) as u32,
        offset_of!(SceneV5, base) as u32,
        offset_of!(SceneV5, imports) as u32,
        offset_of!(SceneV5, import_count) as u32,
        offset_of!(SceneV5, reserved) as u32,
        offset_of!(SceneV5, bounds) as u32,
        offset_of!(SceneV5, bounds_count) as u32,
        offset_of!(SceneV5, reserved1) as u32,
    ];
    let mut actual = [0; 26];
    let status = unsafe { mag_native_scene_v5_layout(actual.as_mut_ptr(), 26) };
    if status != 0 {
        return Err(status);
    }
    if expected != actual {
        return Err(-1);
    }
    Ok(())
}
#[cfg(test)]
mod v5_tests {
    #[test]
    fn malformed_aux_constructor_clears_output_before_rejection() {
        unsafe extern "C" {
            fn mag_native_scene_graphics_create_aux(
                device: *mut std::ffi::c_void,
                desc: *const super::SceneGraphicsV3Desc,
                flags: u32,
                out: *mut super::Pipeline,
            ) -> i32;
        }
        let mut out = super::Pipeline {
            device_generation: 99,
            id: 123,
        };
        assert_eq!(
            unsafe {
                mag_native_scene_graphics_create_aux(
                    std::ptr::null_mut(),
                    std::ptr::null(),
                    0,
                    &mut out,
                )
            },
            -1
        );
        assert_eq!(out.device_generation, 0);
        assert_eq!(out.id, 0);
    }
    #[test]
    fn imported_scene_fields_match_independent_c() {
        super::checked_v5_layout().unwrap();
    }
}
