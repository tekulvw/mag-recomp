//! The ABI records that carry pointers (`DescV3`, `Pass`, `Draw` and the tables they reference),
//! built from an owned arena.
//!
//! [`SceneBuilder`] owns every table and byte range. [`SceneBuilder::view`] borrows it and
//! returns a [`PacketView`] whose `DescV3` points only into the builder and into the view's own
//! heap tables. The pointers stay valid for as long as the view lives, and the view cannot
//! outlive the builder, so the synchronous copy into the native worker always reads live,
//! unchanged memory. Nothing here is `unsafe`.

use crate::packet::{OwnedAttachment, OwnedItem, OwnedPass, PacketAssembler};
use mag_render_abi::packet::{
    Binding, Buffer, BufferUse, Desc, DescV2, DescV3, Dispatch, Draw, Image, ImageView, Item, Pass,
    Pipeline, Program, RasterState, Sampler, SamplerSlot, StencilAttachment, Subresource,
    TextureSlot, TextureUse, Viewport,
};
use mag_render_abi::{
    Attachment, BindingLayout, ShaderStage, Source, TextureLayout, VertexAttribute,
};
use mag_render_behavior::surfaces::NativeViewport;
use std::ffi::c_void;
use std::marker::PhantomData;
use std::mem::size_of;

/// Shader source languages of the packet.
pub const LANGUAGE_MSL: u32 = 1;
pub const LANGUAGE_HLSL: u32 = 2;
pub const LANGUAGE_SPIRV: u32 = 3;
/// View and image dimensions.
pub const DIMENSION_2D: u32 = 2;
pub const DIMENSION_CUBE: u32 = 4;
/// Viewport `known_fields` value for a fully known viewport.
pub const VIEWPORT_KNOWN: u32 = 7;
/// Buffer roles.
pub const BUFFER_BYTES: u32 = 0;
pub const BUFFER_PRODUCED: u32 = 1;

/// An owned program.
#[derive(Clone, Debug, PartialEq)]
pub struct OwnedProgram {
    /// 0 vertex, 1 fragment, 2 compute.
    pub stage: u32,
    pub flags: u32,
    pub language: u32,
    /// Source bytes; for SPIR-V, the words as little-endian bytes.
    pub source: Vec<u8>,
    pub entry: Vec<u8>,
    pub buffers: Vec<BindingLayoutRecord>,
    pub textures: Vec<TextureSlotRecord>,
    pub samplers: Vec<(u32, u32)>,
    pub vertex_locations: Vec<u32>,
    pub threads: [u32; 3],
}

/// `BindingLayout`, owned.
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub struct BindingLayoutRecord {
    pub slot: u32,
    pub access: u32,
    pub minimum_bytes: u64,
    pub runtime_array_stride: u32,
    pub runtime_size_index: u32,
    pub alignment_bytes: u32,
    pub binding_kind: u32,
}

impl BindingLayoutRecord {
    /// A read-only uniform block of `bytes` at `slot`.
    pub fn uniform(slot: u32, bytes: u64) -> Self {
        Self {
            slot,
            access: 1,
            minimum_bytes: bytes,
            runtime_array_stride: 0,
            runtime_size_index: u32::MAX,
            alignment_bytes: 16,
            binding_kind: 1,
        }
    }
}

/// `TextureSlot`, owned.
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub struct TextureSlotRecord {
    pub slot: u32,
    pub dimension: u32,
    pub sample_kind: u32,
}

/// An owned pipeline.
#[derive(Clone, Copy)]
pub struct PipelineRecord {
    pub vertex: u32,
    /// `u32::MAX` for none.
    pub fragment: u32,
    pub stride: u32,
    pub colour_format: u32,
    pub depth_format: u32,
    pub binding_model: u32,
    pub state: RasterState,
}

#[derive(Clone)]
pub struct OwnedPipeline {
    pub record: PipelineRecord,
    pub attributes: Vec<VertexAttribute>,
    pub textures: Vec<TextureLayout>,
}

#[derive(Clone, Debug, PartialEq, Eq)]
pub enum OwnedBuffer {
    Bytes(Vec<u8>),
    /// Produced on the device; only its length is declared.
    Produced(u64),
}

#[derive(Clone, Debug, PartialEq, Eq)]
pub struct OwnedSubresource {
    pub mip: u32,
    pub layer: u32,
    pub width: u32,
    pub height: u32,
    pub depth: u32,
    pub row_bytes: u32,
    pub bytes: Vec<u8>,
}

#[derive(Clone, Debug, PartialEq, Eq)]
pub struct OwnedImage {
    pub format: u32,
    pub dimension: u32,
    pub width: u32,
    pub height: u32,
    pub depth: u32,
    pub layers: u32,
    pub mip_count: u32,
    pub samples: u32,
    /// Empty for an attachment-only image.
    pub subresources: Vec<OwnedSubresource>,
    /// Optional content key (0 = none).
    pub key: u32,
}

impl OwnedImage {
    /// A single-sample, single-mip 2D attachment image without contents.
    pub fn attachment(format: u32, width: u32, height: u32) -> Self {
        Self {
            format,
            dimension: DIMENSION_2D,
            width,
            height,
            depth: 1,
            layers: 1,
            mip_count: 1,
            samples: 1,
            subresources: Vec::new(),
            key: 0,
        }
    }
}

/// An owned draw.
#[derive(Clone, Debug, PartialEq, Eq)]
pub struct OwnedDraw {
    pub source_item: u64,
    pub source_partition: u64,
    pub obligations: u64,
    pub pipeline: u32,
    pub index_count: u32,
    pub vertices: (u32, u64, u64),
    pub indices: (u32, u64, u64),
    /// (stage, slot, buffer, offset, length).
    pub buffers: Vec<(u32, u32, u32, u64, u64)>,
    /// (stage, texture slot, sampler slot, view, sampler).
    pub textures: Vec<(u32, u32, u32, u32, u32)>,
}

/// An owned pass with its per-pass viewport and stencil records (packet v2/v3).
#[derive(Clone)]
pub struct OwnedPassRecord {
    pub source: Source,
    pub colour: Attachment,
    pub depth: Attachment,
    pub stencil: StencilAttachment,
    pub viewport: Viewport,
    pub draws: Vec<OwnedDraw>,
}

/// The absent stencil record of a pass whose depth image has no stencil plane.
pub const NO_STENCIL: StencilAttachment = StencilAttachment {
    image: u32::MAX,
    load: 0,
    store: 0,
    clear_value: 0,
    flags: 0,
    reserved: 0,
};

/// A fully known packet viewport from a mapped native viewport.
pub fn packet_viewport(v: &NativeViewport) -> Viewport {
    Viewport {
        x: v.x,
        y: v.y,
        width: v.width,
        height: v.height,
        min_depth: v.min_depth,
        max_depth: v.max_depth,
        scissor_x: v.scissor[0],
        scissor_y: v.scissor[1],
        scissor_width: v.scissor[2],
        scissor_height: v.scissor[3],
        known_fields: VIEWPORT_KNOWN,
        reserved: 0,
    }
}

/// An owned compute dispatch of the packet's compute prefix: a stage-2 program, its buffer
/// bindings `(slot, buffer, offset, length)` and the workgroup counts. Dispatches run in order,
/// before every pass; a produced buffer is written by exactly one dispatch.
#[derive(Clone, Debug, PartialEq, Eq)]
pub struct OwnedDispatch {
    pub program: u32,
    pub bindings: Vec<(u32, u32, u64, u64)>,
    pub groups: [u32; 3],
}

/// The owned arena of one scene packet.
#[derive(Clone)]
pub struct SceneBuilder {
    pub source: Source,
    /// Bit 0: items and draws may carry unresolved obligations.
    pub flags: u32,
    pub programs: Vec<OwnedProgram>,
    pub pipelines: Vec<OwnedPipeline>,
    pub buffers: Vec<OwnedBuffer>,
    pub images: Vec<OwnedImage>,
    /// Views with their dimension.
    pub views: Vec<(ImageView, u32)>,
    pub samplers: Vec<Sampler>,
    pub dispatches: Vec<OwnedDispatch>,
    pub passes: Vec<OwnedPassRecord>,
    pub items: Vec<Item>,
    pub output_image: u32,
}

fn index(len: usize) -> u32 {
    u32::try_from(len).expect("packet tables are far below u32::MAX")
}

fn view_bits(view: &ImageView) -> [u32; 10] {
    [
        view.image,
        view.base_mip,
        view.mip_count,
        view.base_layer,
        view.layer_count,
        view.remap[0],
        view.remap[1],
        view.remap[2],
        view.remap[3],
        view.reserved,
    ]
}

fn sampler_bits(s: &Sampler) -> Vec<u32> {
    let mut bits = vec![
        (s.known_fields >> 32) as u32,
        s.known_fields as u32,
        s.min_filter,
        s.mag_filter,
        s.mip_filter,
        s.wrap_s,
        s.wrap_t,
        s.wrap_r,
        s.comparison_enabled,
        s.comparison,
    ];
    bits.extend([s.lod_min, s.lod_max, s.lod_bias, s.max_anisotropy].map(f32::to_bits));
    bits.extend(s.border.map(f32::to_bits));
    bits
}

impl SceneBuilder {
    pub fn new(source: Source) -> Self {
        Self {
            source,
            flags: 0,
            programs: Vec::new(),
            pipelines: Vec::new(),
            buffers: Vec::new(),
            images: Vec::new(),
            views: Vec::new(),
            samplers: Vec::new(),
            dispatches: Vec::new(),
            passes: Vec::new(),
            items: Vec::new(),
            output_image: 0,
        }
    }

    pub fn add_program(&mut self, program: OwnedProgram) -> u32 {
        self.programs.push(program);
        index(self.programs.len() - 1)
    }

    pub fn add_pipeline(&mut self, pipeline: OwnedPipeline) -> u32 {
        self.pipelines.push(pipeline);
        index(self.pipelines.len() - 1)
    }

    pub fn add_dispatch(&mut self, dispatch: OwnedDispatch) -> u32 {
        self.dispatches.push(dispatch);
        index(self.dispatches.len() - 1)
    }

    pub fn add_buffer(&mut self, buffer: OwnedBuffer) -> u32 {
        self.buffers.push(buffer);
        index(self.buffers.len() - 1)
    }

    pub fn add_image(&mut self, image: OwnedImage) -> u32 {
        self.images.push(image);
        index(self.images.len() - 1)
    }

    /// Adds a view, de-duplicated by the bits of every field.
    pub fn add_view(&mut self, view: ImageView, dimension: u32) -> u32 {
        if let Some(found) = self
            .views
            .iter()
            .position(|(v, d)| view_bits(v) == view_bits(&view) && *d == dimension)
        {
            return index(found);
        }
        self.views.push((view, dimension));
        index(self.views.len() - 1)
    }

    /// Adds a sampler, de-duplicated by the bits of every field.
    pub fn add_sampler(&mut self, sampler: Sampler) -> u32 {
        if let Some(found) = self
            .samplers
            .iter()
            .position(|s| sampler_bits(s) == sampler_bits(&sampler))
        {
            return index(found);
        }
        self.samplers.push(sampler);
        index(self.samplers.len() - 1)
    }

    /// Converts the assembler's passes and items, attaching each pass's viewport. Pass sources
    /// are this packet's source.
    pub fn add_assembled(
        &mut self,
        assembled: PacketAssembler<OwnedDraw>,
        viewports: &[Viewport],
    ) -> Result<(), &'static str> {
        if viewports.len() != assembled.passes.len() {
            return Err("one viewport per pass");
        }
        let base = index(self.passes.len());
        for (pass, viewport) in assembled.passes.into_iter().zip(viewports) {
            let OwnedPass {
                colour,
                depth,
                stencil,
                draws,
                ..
            } = pass;
            self.passes.push(OwnedPassRecord {
                source: self.source,
                colour: colour.to_abi(),
                depth: depth.to_abi(),
                stencil: stencil.unwrap_or(NO_STENCIL),
                viewport: *viewport,
                draws,
            });
        }
        for OwnedItem {
            pass,
            kind,
            source_item,
        } in assembled.items
        {
            self.items.push(Item {
                pass: base + pass,
                reserved: kind,
                source_item,
                unresolved_obligations: 0,
            });
        }
        Ok(())
    }

    /// Borrows the arena as ABI records.
    pub fn view(&self) -> PacketView<'_> {
        PacketView::new(self)
    }
}

/// The ABI records of one packet, borrowed from a [`SceneBuilder`].
pub struct PacketView<'a> {
    _programs_buffers: Vec<Vec<BindingLayout>>,
    _programs_textures: Vec<Vec<TextureSlot>>,
    _programs_samplers: Vec<Vec<SamplerSlot>>,
    _programs: Vec<Program>,
    _pipelines: Vec<Pipeline>,
    _buffers: Vec<Buffer>,
    _subresources: Vec<Vec<Subresource>>,
    _images: Vec<Image>,
    _views: Vec<ImageView>,
    _dimensions: Vec<u32>,
    _draw_buffers: Vec<Vec<Vec<Binding>>>,
    _draw_textures: Vec<Vec<Vec<TextureUse>>>,
    _draws: Vec<Vec<Draw>>,
    _passes: Vec<Pass>,
    _dispatch_bindings: Vec<Vec<Binding>>,
    _dispatches: Vec<Dispatch>,
    _viewports: Vec<Viewport>,
    _stencils: Vec<StencilAttachment>,
    desc: DescV3,
    _arena: PhantomData<&'a SceneBuilder>,
}

fn use_of((buffer, offset, length): (u32, u64, u64)) -> BufferUse {
    BufferUse {
        buffer,
        reserved: 0,
        offset,
        length,
    }
}

impl<'a> PacketView<'a> {
    fn new(b: &'a SceneBuilder) -> Self {
        let programs_buffers: Vec<Vec<BindingLayout>> = b
            .programs
            .iter()
            .map(|p| {
                p.buffers
                    .iter()
                    .map(|r| BindingLayout {
                        slot: r.slot,
                        access: r.access,
                        minimum_bytes: r.minimum_bytes,
                        runtime_array_stride: r.runtime_array_stride,
                        runtime_size_index: r.runtime_size_index,
                        alignment_bytes: r.alignment_bytes,
                        binding_kind: r.binding_kind,
                    })
                    .collect()
            })
            .collect();
        let programs_textures: Vec<Vec<TextureSlot>> = b
            .programs
            .iter()
            .map(|p| {
                p.textures
                    .iter()
                    .map(|t| TextureSlot {
                        slot: t.slot,
                        dimension: t.dimension,
                        sample_kind: t.sample_kind,
                        arrayed: 0,
                        multisampled: 0,
                        reserved: 0,
                    })
                    .collect()
            })
            .collect();
        let programs_samplers: Vec<Vec<SamplerSlot>> = b
            .programs
            .iter()
            .map(|p| {
                p.samplers
                    .iter()
                    .map(|(slot, comparison)| SamplerSlot {
                        slot: *slot,
                        comparison: *comparison,
                    })
                    .collect()
            })
            .collect();
        let programs: Vec<Program> = b
            .programs
            .iter()
            .enumerate()
            .map(|(i, p)| Program {
                stage: p.stage,
                reserved: p.flags,
                shader: ShaderStage {
                    source: p.source.as_ptr().cast::<c_void>(),
                    source_length: if p.language == LANGUAGE_SPIRV {
                        (p.source.len() / 4) as u64 * 4
                    } else {
                        p.source.len() as u64
                    },
                    entry: p.entry.as_ptr().cast(),
                    entry_length: p.entry.len() as u32,
                    language: p.language,
                },
                buffers: programs_buffers[i].as_ptr(),
                buffer_count: programs_buffers[i].len() as u32,
                texture_count: programs_textures[i].len() as u32,
                textures: programs_textures[i].as_ptr(),
                samplers: programs_samplers[i].as_ptr(),
                sampler_count: programs_samplers[i].len() as u32,
                vertex_location_count: p.vertex_locations.len() as u32,
                vertex_locations: p.vertex_locations.as_ptr(),
                threads: p.threads,
                reserved1: 0,
            })
            .collect();
        let pipelines: Vec<Pipeline> = b
            .pipelines
            .iter()
            .map(|p| Pipeline {
                vertex_program: p.record.vertex,
                fragment_program: p.record.fragment,
                vertex_stride: p.record.stride,
                attribute_count: p.attributes.len() as u32,
                attributes: p.attributes.as_ptr(),
                color_format: p.record.colour_format,
                depth_format: p.record.depth_format,
                binding_model: p.record.binding_model,
                reserved: 0,
                textures: p.textures.as_ptr(),
                texture_count: p.textures.len() as u32,
                reserved1: 0,
                state: p.record.state,
            })
            .collect();
        let buffers: Vec<Buffer> = b
            .buffers
            .iter()
            .map(|buffer| match buffer {
                OwnedBuffer::Bytes(bytes) => Buffer {
                    role: BUFFER_BYTES,
                    reserved: 0,
                    bytes: bytes.as_ptr().cast(),
                    length: bytes.len() as u64,
                },
                OwnedBuffer::Produced(length) => Buffer {
                    role: BUFFER_PRODUCED,
                    reserved: 0,
                    bytes: std::ptr::null(),
                    length: *length,
                },
            })
            .collect();
        let subresources: Vec<Vec<Subresource>> = b
            .images
            .iter()
            .map(|image| {
                image
                    .subresources
                    .iter()
                    .map(|s| Subresource {
                        mip: s.mip,
                        layer: s.layer,
                        width: s.width,
                        height: s.height,
                        depth: s.depth,
                        row_bytes: s.row_bytes,
                        bytes: s.bytes.as_ptr().cast(),
                        length: s.bytes.len() as u64,
                    })
                    .collect()
            })
            .collect();
        let images: Vec<Image> = b
            .images
            .iter()
            .enumerate()
            .map(|(i, image)| Image {
                format: image.format,
                dimension: image.dimension,
                width: image.width,
                height: image.height,
                depth: image.depth,
                layers: image.layers,
                mip_count: image.mip_count,
                samples: image.samples,
                subresources: subresources[i].as_ptr(),
                subresource_count: subresources[i].len() as u32,
                reserved: image.key,
            })
            .collect();
        let views: Vec<ImageView> = b.views.iter().map(|(v, _)| *v).collect();
        let dimensions: Vec<u32> = b.views.iter().map(|(_, d)| *d).collect();
        let draw_buffers: Vec<Vec<Vec<Binding>>> = b
            .passes
            .iter()
            .map(|pass| {
                pass.draws
                    .iter()
                    .map(|d| {
                        d.buffers
                            .iter()
                            .map(|&(stage, slot, buffer, offset, length)| Binding {
                                stage,
                                slot,
                                view: use_of((buffer, offset, length)),
                            })
                            .collect()
                    })
                    .collect()
            })
            .collect();
        let draw_textures: Vec<Vec<Vec<TextureUse>>> = b
            .passes
            .iter()
            .map(|pass| {
                pass.draws
                    .iter()
                    .map(|d| {
                        d.textures
                            .iter()
                            .map(
                                |&(stage, texture_slot, sampler_slot, view, sampler)| TextureUse {
                                    stage,
                                    texture_slot,
                                    sampler_slot,
                                    view,
                                    sampler,
                                    reserved: 0,
                                },
                            )
                            .collect()
                    })
                    .collect()
            })
            .collect();
        let draws: Vec<Vec<Draw>> = b
            .passes
            .iter()
            .enumerate()
            .map(|(p, pass)| {
                pass.draws
                    .iter()
                    .enumerate()
                    .map(|(i, d)| Draw {
                        source_item: d.source_item,
                        source_partition: d.source_partition,
                        unresolved_obligations: d.obligations,
                        pipeline: d.pipeline,
                        index_count: d.index_count,
                        vertices: use_of(d.vertices),
                        indices: use_of(d.indices),
                        buffers: draw_buffers[p][i].as_ptr(),
                        buffer_count: draw_buffers[p][i].len() as u32,
                        texture_count: draw_textures[p][i].len() as u32,
                        textures: draw_textures[p][i].as_ptr(),
                    })
                    .collect()
            })
            .collect();
        let passes: Vec<Pass> = b
            .passes
            .iter()
            .enumerate()
            .map(|(p, pass)| Pass {
                source: pass.source,
                color: pass.colour,
                depth: pass.depth,
                draws: draws[p].as_ptr(),
                draw_count: draws[p].len() as u32,
                reserved: 0,
            })
            .collect();
        let viewports: Vec<Viewport> = b.passes.iter().map(|p| p.viewport).collect();
        let stencils: Vec<StencilAttachment> = b.passes.iter().map(|p| p.stencil).collect();
        let dispatch_bindings: Vec<Vec<Binding>> = b
            .dispatches
            .iter()
            .map(|d| {
                d.bindings
                    .iter()
                    .map(|&(slot, buffer, offset, length)| Binding {
                        stage: 2,
                        slot,
                        view: use_of((buffer, offset, length)),
                    })
                    .collect()
            })
            .collect();
        let dispatches: Vec<Dispatch> = b
            .dispatches
            .iter()
            .zip(&dispatch_bindings)
            .map(|(d, bindings)| Dispatch {
                program: d.program,
                binding_count: bindings.len() as u32,
                bindings: bindings.as_ptr(),
                groups: d.groups,
                reserved: 0,
            })
            .collect();
        let base = Desc {
            abi_version: 1,
            struct_size: size_of::<Desc>() as u32,
            flags: b.flags,
            reserved: 0,
            source: b.source,
            intended_items: b.items.len() as u64,
            programs: programs.as_ptr(),
            program_count: programs.len() as u32,
            pipeline_count: pipelines.len() as u32,
            pipelines: pipelines.as_ptr(),
            buffers: buffers.as_ptr(),
            buffer_count: buffers.len() as u32,
            image_count: images.len() as u32,
            images: images.as_ptr(),
            views: views.as_ptr(),
            view_count: views.len() as u32,
            sampler_count: b.samplers.len() as u32,
            samplers: b.samplers.as_ptr(),
            dispatches: if dispatches.is_empty() {
                std::ptr::null()
            } else {
                dispatches.as_ptr()
            },
            dispatch_count: dispatches.len() as u32,
            pass_count: passes.len() as u32,
            passes: passes.as_ptr(),
            items: b.items.as_ptr(),
            item_count: b.items.len() as u32,
            output_image: b.output_image,
            target: std::ptr::null(),
            frame: std::ptr::null(),
        };
        let v2 = DescV2 {
            abi_version: 2,
            struct_size: size_of::<DescV2>() as u32,
            base,
            viewports: viewports.as_ptr(),
            viewport_count: viewports.len() as u32,
            reserved: 0,
            view_dimensions: dimensions.as_ptr(),
            view_dimension_count: dimensions.len() as u32,
            reserved1: 0,
        };
        let desc = DescV3 {
            abi_version: 3,
            struct_size: size_of::<DescV3>() as u32,
            base: v2,
            stencils: stencils.as_ptr(),
            stencil_count: stencils.len() as u32,
            reserved: 0,
        };
        Self {
            _programs_buffers: programs_buffers,
            _programs_textures: programs_textures,
            _programs_samplers: programs_samplers,
            _programs: programs,
            _pipelines: pipelines,
            _buffers: buffers,
            _subresources: subresources,
            _images: images,
            _views: views,
            _dimensions: dimensions,
            _draw_buffers: draw_buffers,
            _draw_textures: draw_textures,
            _draws: draws,
            _passes: passes,
            _dispatch_bindings: dispatch_bindings,
            _dispatches: dispatches,
            _viewports: viewports,
            _stencils: stencils,
            desc,
            _arena: PhantomData,
        }
    }

    /// The packet descriptor. Every pointer in it refers to memory owned by this view or by the
    /// builder it borrows.
    pub fn desc(&self) -> &DescV3 {
        &self.desc
    }
}

/// Encodes an `OwnedAttachment` (from the plan assembler) as the ABI record.
pub fn attachment(a: OwnedAttachment) -> Attachment {
    a.to_abi()
}
