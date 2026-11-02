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
unsafe extern "C" {
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
        let extended = v4
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
    pub(super) unsafe fn submit(
        &self,
        device: *mut c_void,
        scene: &Scene,
        samplers: &[scene_sampler::Handle],
        target: Option<&crate::host_image::NativeImage>,
        frame: Option<&crate::host_frame::HostFrame>,
        out: &mut Ticket,
    ) -> i32 {
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
