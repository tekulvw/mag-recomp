//! Actual retained-Main material source capture, before Main/SPU preparation.
//! The result owns source bytes/structure, not a complete native material or a
//! streaming lifetime certificate. No original producer is suppressed.
// The material capture's parts, under the names this module uses. Setup
// banks are copied by the behaviour crate (`material-setup.md`).
pub use crate::{
    material_capture::MaterialCaptureError,
    material_finish as finish, material_group_twelve as group_twelve,
    material_textures::{self as textures, OwnedDescriptorSource, TextureObjectSource},
    material_values as values,
};
pub use mag_render_adapter_core::material_pipeline as pipeline;
pub use mag_render_behavior::setup;

/// The capture's located reader as a behaviour [`GuestMemory`] over main
/// memory IO offsets. Each read is the same charged located copy the
/// material reader makes for any other request.
struct MainLocated<'r, F>(std::cell::RefCell<&'r mut F>);
impl<F> mag_render_behavior::guest::GuestMemory for MainLocated<'_, F>
where
    F: FnMut(textures::LocatedTextureRequest) -> Result<Vec<u8>, textures::TextureError>,
{
    fn read(
        &self,
        address: u32,
        length: u32,
    ) -> Result<Vec<u8>, mag_render_behavior::guest::SourceError> {
        (self.0.borrow_mut())(textures::LocatedTextureRequest {
            location: textures::TextureLocation::Main,
            offset: address,
            bytes: length as usize,
        })
        .map_err(|error| mag_render_behavior::guest::SourceError {
            address,
            length,
            reason: format!("{error:?}"),
        })
    }
}
use crate::{
    cohort::OwnedVisibilityCohort,
    cohort_resources::{MainPassSnapshot, OwnedCohortResources},
    error::Error,
    fragment_builtin_rows::prepare_fragment_builtin_rows,
    guest_memory::GuestSnapshot,
    material_constants::{
        BuiltinError, BuiltinPassValues, ConstantBudget, ConstantError, ConstantRowWrite,
        CurrentAttributeUpdate, FragmentBuiltinRequest, FragmentConstantLayout,
        ParameterAssignment, ParameterError, current_attribute_updates,
        decode_fragment_builtin_requests, decode_ordinary_parameters, ordinary_fragment_rows,
    },
    material_programs::{
        FragmentSourceReference, ProgramBudget, ProgramError, TechniquePrograms,
        decode_technique_programs,
    },
    material_sources::{MaterialSource, SourceError, TechniqueResourceBase, TechniqueSource},
    material_state::{PrimaryStateError, PrimaryStateInput},
    mesh_sources::{MeshSource, MeshSourceError},
    partition_inputs::StreamRequest,
};

#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub struct MaterialPassSources {
    pub source_pass_address: u32,
    pub asset_bank: u32,
    pub render_mode: i32,
    pub technique_override_mask: u32,
}
pub struct MaterialPassInput<'a> {
    pub parameters: MaterialPassSources,
    pub snapshot: Option<MainPassSnapshot<'a>>,
}
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub struct MaterialCopyBudget {
    pub bytes: usize,
    pub reads: usize,
    pub items: usize,
}
#[derive(Clone, Debug, PartialEq, Eq)]
pub enum MaterialItemError {
    Capture(MaterialCaptureError),
    Material(SourceError),
    Mesh(MeshSourceError),
    State(PrimaryStateError),
    Program(ProgramError),
    UnqualifiedOverride(u32),
    UnqualifiedFragmentReference,
    ParameterBounds,
}
#[derive(Clone, Debug, PartialEq, Eq)]
pub enum ProducerTextureError {
    MissingDrawSource,
    MissingAuxiliary,
    Bounds,
    Capture(MaterialCaptureError),
    Texture(textures::TextureError),
}
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub enum GeometryAttributeSix {
    Unchanged,
    Current([u32; 4]),
    /// The source enables an optional slot6 array. Its data must come from an
    /// actual owned array binding; a carried current value cannot stand in.
    Stream,
}
pub struct OwnedMaterialSources {
    pub material_address: u32,
    pub primary_state: PrimaryStateInput,
    pub technique: TechniqueSource,
    pub programs: TechniquePrograms,
    pub fragment_program_address: u32,
    pub fragment_template: Vec<u8>,
    /// Seeds/structure only until actual ordinary/builtin bindings supply the
    /// logical row domain. A failure is retained, never an empty valid shader.
    pub fragment_seed_layout: Result<FragmentConstantLayout, ConstantError>,
    pub parameters: Result<Vec<ParameterAssignment>, ParameterError>,
    pub ordinary_fragment_rows: Result<Vec<ConstantRowWrite>, ParameterError>,
    pub current_attribute_updates: Result<Vec<CurrentAttributeUpdate>, ParameterError>,
    /// Main static-partition slot6 effect, after material binding. This never
    /// manufactures the optional stream contents.
    pub geometry_attribute_six: Result<GeometryAttributeSix, MaterialCaptureError>,
    /// Builtin phase remains distinct from ordinary assignments until the
    /// selected/primary role and complete writer order are admitted by a draw.
    pub builtin_requests: Result<Vec<FragmentBuiltinRequest>, BuiltinError>,
    pub fragment_builtin_rows: Result<Vec<ConstantRowWrite>, BuiltinError>,
    pub textures: Vec<TextureObjectSource>,
    pub texture_bindings: Result<
        Vec<Result<textures::OwnedTextureBinding, textures::TextureError>>,
        textures::TextureError,
    >,
    /// Actual per-item producer writes after binder state, including units12..14.
    pub producer_texture_bindings: Result<Vec<textures::OwnedTextureBinding>, ProducerTextureError>,
    pub draw_sources: Result<values::MaterialDrawSources, values::ValueError>,
}
pub struct MaterialItemCapture {
    pub ordinal: usize,
    pub sources: Result<OwnedMaterialSources, MaterialItemError>,
}
pub struct OwnedMainMaterials {
    pub pass: MaterialPassSources,
    pub source_pass_control: u32,
    pub finish_sources: Result<finish::OwnedMainFinish, finish::FinishCaptureError>,
    pub group_twelve_sources:
        Result<Option<group_twelve::OwnedGroupTwelveTextures>, group_twelve::GroupTwelveError>,
    pub setup_sources: Result<pipeline::MainPipelineSources, pipeline::MainSetupError>,
    /// Bounded compatibility evidence from actual retained PPE command banks.
    /// These bytes are not preparation-removal proof or completed GPU work.
    pub retained_setup_banks: Option<setup::RetainedMainSetupBanks>,
    pub override_mode: u32,
    pub binding_payload_address: u32,
    pub fallback_descriptors: [Option<OwnedDescriptorSource>; 6],
    pub producer_texture_sources: [Option<OwnedDescriptorSource>; 4],
    /// Only values actually requested by active, qualified fragment builtins.
    pub builtin_pass_values: BuiltinPassValues,
    pub items: Vec<MaterialItemCapture>,
    pub source_bytes: usize,
    pub source_reads: usize,
}
/// The material reader plus the capture's located reader, as one texture source.
struct ReaderSource<'r, 'a, F, L> {
    reader: &'r mut Reader<'a, F>,
    located: &'r mut L,
}
impl<F, L> textures::LocatedSource for ReaderSource<'_, '_, F, L>
where
    F: FnMut(StreamRequest) -> Result<Vec<u8>, Error>,
    L: FnMut(textures::LocatedTextureRequest) -> Result<Vec<u8>, textures::TextureError>,
{
    fn read(
        &mut self,
        request: textures::LocatedTextureRequest,
    ) -> Result<Vec<u8>, textures::TextureError> {
        self.reader.copy_located(request, self.located)
    }
    fn validate(
        &mut self,
        request: textures::LocatedTextureRequest,
        expected: &[u8],
    ) -> Result<bool, textures::TextureError> {
        self.reader.validate_located(request, expected)
    }
}
struct Reader<'a, F> {
    read: &'a mut F,
    pass_snapshot: Option<MainPassSnapshot<'a>>,
    bytes: usize,
    requested_bytes: usize,
    reads: usize,
    budget: MaterialCopyBudget,
}
impl<F: FnMut(StreamRequest) -> Result<Vec<u8>, Error>> Reader<'_, F> {
    fn charge_memory(&mut self, bytes: usize) -> Result<(), MaterialCaptureError> {
        self.requested_bytes = self
            .requested_bytes
            .checked_add(bytes)
            .filter(|n| *n <= self.budget.bytes)
            .ok_or(MaterialCaptureError::Budget)?;
        Ok(())
    }
    fn charge(&mut self, bytes: usize) -> Result<(), MaterialCaptureError> {
        let total = self
            .requested_bytes
            .checked_add(bytes)
            .ok_or(MaterialCaptureError::Budget)?;
        if total > self.budget.bytes || self.reads >= self.budget.reads {
            return Err(MaterialCaptureError::Budget);
        }
        self.requested_bytes = total;
        self.reads += 1;
        Ok(())
    }
    fn copy(&mut self, address: u32, bytes: usize) -> Result<Vec<u8>, MaterialCaptureError> {
        let request = StreamRequest { address, bytes };
        if address == 0
            || u64::from(address)
                .checked_add(bytes as u64)
                .is_none_or(|end| end > 1u64 << 32)
        {
            return Err(MaterialCaptureError::Extent(request));
        }
        if let Some(bytes) = self.pass_snapshot.and_then(|s| s.slice(address, bytes)) {
            self.charge_memory(bytes.len())?;
            return GuestSnapshot::new(address, bytes)
                .and_then(|s| s.copy_bytes(u64::from(address), bytes.len()))
                .map_err(|error| MaterialCaptureError::Source { request, error });
        }
        // Failed callbacks consume the same attempt/byte budget. Otherwise an
        // all-invalid population could make unbounded failed reads while every
        // successful-read counter remained zero.
        self.charge(bytes)?;
        let owned = (self.read)(request)
            .map_err(|error| MaterialCaptureError::Source { request, error })?;
        if owned.len() != bytes {
            return Err(MaterialCaptureError::Extent(request));
        }
        self.bytes += bytes;
        Ok(owned)
    }
    fn copy_located(
        &mut self,
        request: textures::LocatedTextureRequest,
        read: &mut impl FnMut(
            textures::LocatedTextureRequest,
        ) -> Result<Vec<u8>, textures::TextureError>,
    ) -> Result<Vec<u8>, textures::TextureError> {
        self.charge(request.bytes)
            .map_err(|_| textures::TextureError::Budget)?;
        let owned = read(request)?;
        if owned.len() != request.bytes {
            return Err(textures::TextureError::Extent);
        }
        self.bytes += request.bytes;
        Ok(owned)
    }
    /// The copy-free twin of `copy_located` for a span whose expected bytes
    /// are known: on an in-place match it charges and counts exactly what
    /// `copy_located` would; otherwise nothing is charged (`Ok(false)`).
    fn validate_located(
        &mut self,
        request: textures::LocatedTextureRequest,
        expected: &[u8],
    ) -> Result<bool, textures::TextureError> {
        if crate::capture_view::located_view_equals(request, expected) != Some(true) {
            return Ok(false);
        }
        self.charge(request.bytes)
            .map_err(|_| textures::TextureError::Budget)?;
        self.bytes += request.bytes;
        Ok(true)
    }
    fn field(&mut self, pass: u32, offset: u32) -> Result<u32, MaterialCaptureError> {
        let at = pass
            .checked_add(offset)
            .ok_or(MaterialCaptureError::InvalidPass)?;
        if let Some(bytes) = self.pass_snapshot.and_then(|s| s.slice(at, 4)) {
            return Ok(u32::from_be_bytes(
                bytes.try_into().expect("checked pass word"),
            ));
        }
        Ok(u32::from_be_bytes(
            self.copy(at, 4)?
                .try_into()
                .map_err(|_| MaterialCaptureError::InvalidPass)?,
        ))
    }
    fn descriptor(
        &mut self,
        address: u32,
        bytes: usize,
    ) -> Result<Option<OwnedDescriptorSource>, MaterialCaptureError> {
        if address == 0 {
            return Ok(None);
        }
        Ok(Some(OwnedDescriptorSource {
            address,
            bytes: self.copy(address, bytes)?,
        }))
    }
}
fn word(bytes: &[u8], at: usize) -> Result<u32, MaterialItemError> {
    Ok(u32::from_be_bytes(
        bytes
            .get(
                at..at
                    .checked_add(4)
                    .ok_or(MaterialItemError::ParameterBounds)?,
            )
            .ok_or(MaterialItemError::ParameterBounds)?
            .try_into()
            .map_err(|_| MaterialItemError::ParameterBounds)?,
    ))
}
fn half(bytes: &[u8], at: usize) -> Result<u16, MaterialItemError> {
    Ok(u16::from_be_bytes(
        bytes
            .get(
                at..at
                    .checked_add(2)
                    .ok_or(MaterialItemError::ParameterBounds)?,
            )
            .ok_or(MaterialItemError::ParameterBounds)?
            .try_into()
            .map_err(|_| MaterialItemError::ParameterBounds)?,
    ))
}
struct ProducerTextureInputs<'a> {
    material_flags: u32,
    binding_mask: u8,
    mbi: &'a [u8; 32],
    pass: u32,
    parameters: &'a [Option<OwnedDescriptorSource>; 4],
}
fn producer_texture_bindings<F: FnMut(StreamRequest) -> Result<Vec<u8>, Error>>(
    input: ProducerTextureInputs<'_>,
    reader: &mut Reader<'_, F>,
    remaining_decoded_bytes: &mut usize,
    image_cache: &mut textures::TextureImageCache,
    located_read: &mut impl FnMut(
        textures::LocatedTextureRequest,
    ) -> Result<Vec<u8>, textures::TextureError>,
) -> Result<Vec<textures::OwnedTextureBinding>, ProducerTextureError> {
    let ProducerTextureInputs {
        material_flags,
        binding_mask,
        mbi,
        pass,
        parameters,
    } = input;
    use ProducerTextureError as E;
    let mut selected: Vec<(usize, u8, u32, [u8; 32])> = Vec::new();
    if material_flags & 16 == 0 {
        let address = reader.field(pass, 0x1c4).map_err(E::Capture)?;
        if let Some(descriptor) = reader.descriptor(address, 32).map_err(E::Capture)? {
            selected.push((
                2,
                14,
                address,
                descriptor.bytes.try_into().map_err(|_| E::Bounds)?,
            ));
        }
    } else if binding_mask & 7 != 0 {
        if u32::from_be_bytes(mbi[24..28].try_into().unwrap()) & 1 == 0 {
            return Err(E::MissingAuxiliary);
        }
        let addresses =
            [9, 16, 20].map(|at| u32::from_be_bytes(mbi[at..at + 4].try_into().unwrap()) & !15);
        let mut records: [Option<Vec<u8>>; 3] = [None, None, None];
        let alternate = if binding_mask & 1 != 0 {
            reader.field(pass, 0x21c).map_err(E::Capture)? >> 24 != 0
        } else {
            false
        };
        for (unit, parameter) in parameters.iter().enumerate().take(3) {
            if binding_mask & (1 << unit) == 0 {
                continue;
            }
            let mut index = unit;
            if unit == 0 {
                if !alternate {
                    records[0] = Some(reader.copy(addresses[0], 96).map_err(E::Capture)?);
                }
                if alternate || records[0].as_ref().is_some_and(|r| r[16] != 2) {
                    index = 2;
                }
            }
            if records[index].is_none() {
                records[index] = Some(reader.copy(addresses[index], 96).map_err(E::Capture)?);
            }
            let record = records[index].as_ref().ok_or(E::MissingAuxiliary)?;
            let (address, raw) = if record[16] == 2 {
                (
                    addresses[index].checked_add(48).ok_or(E::Bounds)?,
                    record[48..80].try_into().map_err(|_| E::Bounds)?,
                )
            } else {
                let descriptor = parameter
                    .as_ref()
                    .ok_or(E::Texture(textures::TextureError::MissingDescriptor))?;
                (
                    descriptor.address,
                    descriptor
                        .bytes
                        .as_slice()
                        .try_into()
                        .map_err(|_| E::Bounds)?,
                )
            };
            selected.push((index, 12 + unit as u8, address, raw));
        }
    }
    let mut bindings = Vec::new();
    bindings
        .try_reserve_exact(selected.len())
        .map_err(|_| E::Texture(textures::TextureError::Allocation))?;
    for (record_index, unit, address, raw) in selected {
        let descriptor =
            std::array::from_fn(|i| u32::from_be_bytes(raw[i * 4..i * 4 + 4].try_into().unwrap()));
        let (selection, image) = if textures::producer_asset_descriptor(descriptor) {
            (
                textures::DescriptorSelection::ProducerAsset(address),
                textures::TexturePayload::Cpu(textures::copy_texture_source_image_from(
                    descriptor,
                    remaining_decoded_bytes,
                    image_cache,
                    &mut ReaderSource {
                        reader,
                        located: located_read,
                    },
                )),
            )
        } else {
            (
                textures::DescriptorSelection::Producer(address),
                textures::TexturePayload::NativeProducer,
            )
        };
        bindings.push(textures::OwnedTextureBinding {
            record_index,
            unit,
            selection,
            descriptor,
            inherited_border_color: None,
            image,
        });
    }
    Ok(bindings)
}
// Only the static partition path forwards the selected material's flag 0x40;
// group 0 and the ordinary and skinning paths forward zero. A descriptor whose
// byte 16 is 2 selects a stream, which takes priority over the current-value
// write. The capture caller has already required selected == primary
// (override_mode == 0).
fn geometry_attribute_six<F: FnMut(StreamRequest) -> Result<Vec<u8>, Error>>(
    mesh: &MeshSource,
    selected_flags: u32,
    mbi: &[u8; 32],
    reader: &mut Reader<'_, F>,
) -> Result<GeometryAttributeSix, MaterialCaptureError> {
    use crate::geometry_bindings::{SlotSixSource, slot_six_source};
    match slot_six_source(
        mesh.material_group(),
        u32::from_be_bytes(mesh.bytes()[0x9c..0xa0].try_into().unwrap()),
        selected_flags,
        mbi,
    ) {
        SlotSixSource::Unchanged => return Ok(GeometryAttributeSix::Unchanged),
        SlotSixSource::Current => (),
        SlotSixSource::Descriptor(descriptor) => {
            let bytes = reader.copy(descriptor, 64)?;
            if bytes[16] == 2 {
                return Ok(GeometryAttributeSix::Stream);
            }
        }
    }
    Ok(GeometryAttributeSix::Current([
        0,
        0x3f80_0000,
        0,
        0x3f00_0000,
    ]))
}
fn draw_sources<F: FnMut(StreamRequest) -> Result<Vec<u8>, Error>>(
    material: &[u8],
    technique: TechniqueSource,
    mesh: &MeshSource,
    item: &crate::cohort::CohortItem,
    pass: u32,
    pass_control: u32,
    reader: &mut Reader<'_, F>,
) -> Result<values::MaterialDrawSources, values::ValueError> {
    use values::{MaterialDrawSources, ParameterColumn, ValueError};
    let transform = item.transform.ok_or(ValueError::MissingTransform)?;
    let read_word = |bytes: &[u8], at: usize| word(bytes, at).map_err(|_| ValueError::SourceBounds);
    let table = usize::from(half(material, 0x48).map_err(|_| ValueError::SourceBounds)?);
    let at = table
        .checked_add(technique.selector as usize * 32)
        .ok_or(ValueError::SourceBounds)?;
    let technique_record: [u8; 32] = material
        .get(at..at + 32)
        .ok_or(ValueError::SourceBounds)?
        .try_into()
        .map_err(|_| ValueError::SourceBounds)?;
    let flags = u64::from_be_bytes(technique_record[8..16].try_into().unwrap());
    let material_flags = read_word(material, 0x58)?;
    let view = if flags & 64 != 0 {
        let address = pass.checked_add(0x80).ok_or(ValueError::SourceBounds)?;
        let bytes = reader.copy(address, 64).map_err(ValueError::Capture)?;
        Some(std::array::from_fn(|r| {
            std::array::from_fn(|c| {
                u32::from_be_bytes(
                    bytes[r * 16 + c * 4..r * 16 + c * 4 + 4]
                        .try_into()
                        .unwrap(),
                )
            })
        }))
    } else {
        None
    };
    let atlas_dimensions = if flags & (1 << 29) != 0 {
        if material_flags & 0x10 == 0 || read_word(&item.mbi, 24)? & 1 == 0 {
            return Err(ValueError::MissingAtlas);
        }
        let address = read_word(&item.mbi, 9)? & !15;
        let packed = reader.field(address, 72).map_err(ValueError::Capture)?;
        Some([packed >> 16, packed & 65535])
    } else {
        None
    };
    let parameter_column = if flags & (1 << 27) != 0 {
        let index = (read_word(&item.mbi, 24)? >> 15) & 0x7ff;
        let base = if index == 0 {
            0
        } else {
            reader.field(pass, 0x140).map_err(ValueError::Capture)?
        };
        if base == 0 || index == 0 {
            ParameterColumn::NullSource
        } else {
            let address = base
                .checked_add(128 * ((index - 1) >> 2))
                .ok_or(ValueError::SourceBounds)?;
            let bytes = reader.copy(address, 128).map_err(ValueError::Capture)?;
            let column = ((index - 1) & 3) as usize;
            ParameterColumn::Values(std::array::from_fn(|r| {
                u32::from_be_bytes(
                    bytes[r * 16 + column * 4..r * 16 + column * 4 + 4]
                        .try_into()
                        .unwrap(),
                )
            }))
        }
    } else {
        ParameterColumn::Unused
    };
    let depth_adjustment = if mesh.material_group() == 11 {
        Some([
            reader.field(pass, 0x68).map_err(ValueError::Capture)?,
            reader.field(pass, 0x1dc).map_err(ValueError::Capture)?,
        ])
    } else {
        None
    };
    let category = u32::from(item.mbi[4] & 7).min(2);
    let class = *material.get(0x56).ok_or(ValueError::SourceBounds)?;
    let alternate = class.wrapping_sub(1) <= 1;
    let mut distance = reader
        .field(pass, if alternate { 0x11c } else { 0x118 } + category * 4)
        .map_err(ValueError::Capture)?;
    let mut denominator = reader
        .field(pass, 0x124 + category * 4)
        .map_err(ValueError::Capture)?;
    let force = alternate && material_flags & 0x8000 != 0;
    if force {
        distance = [30.0f32.to_bits(), 100.0f32.to_bits(), 200.0f32.to_bits()][category as usize];
        denominator = 15.0f32.to_bits();
    }
    let inverse = if !force && (denominator & 0x80000000 != 0 || denominator & 0x7f800000 == 0) {
        0x42c80000
    } else {
        crate::material_constants::source_ratio(0x3f800000, denominator)
    };
    let control = match mesh.material_group() {
        8 | 9 | 12 | 13 => false,
        0 | 10 => true,
        _ => pass_control != 5,
    };
    let fourth = reader.field(pass, 0xd0).map_err(ValueError::Capture)?;
    let mesh_constant = std::array::from_fn(|i| {
        u32::from_be_bytes(mesh.bytes()[0x20 + i * 4..0x24 + i * 4].try_into().unwrap())
    });
    Ok(MaterialDrawSources {
        technique: technique_record,
        mesh_group: mesh.material_group(),
        transform,
        mesh_constant,
        view,
        atlas_dimensions,
        parameter_column,
        depth_adjustment,
        pass_control,
        binding_scalar: [
            distance,
            inverse,
            if control { 0 } else { 0x3f800000 },
            fourth,
        ],
    })
}
fn texture_sources<F: FnMut(StreamRequest) -> Result<Vec<u8>, Error>>(
    material: &[u8],
    selector: u32,
    chunk: &[u8],
    reader: &mut Reader<'_, F>,
) -> Result<Vec<TextureObjectSource>, MaterialItemError> {
    let count = usize::from(
        *material
            .get(0x55)
            .ok_or(MaterialItemError::ParameterBounds)?,
    );
    let technique = usize::from(half(material, 0x48)?)
        .checked_add(selector as usize * 32)
        .ok_or(MaterialItemError::ParameterBounds)?;
    let map = *material
        .get(technique + 1)
        .ok_or(MaterialItemError::ParameterBounds)?;
    let first = usize::from(
        *material
            .get(technique + 2)
            .ok_or(MaterialItemError::ParameterBounds)?,
    );
    if map >= 32 {
        return Err(MaterialItemError::ParameterBounds);
    }
    if first >= count {
        return Ok(Vec::new());
    }
    let records = material
        .get(96..96 + count * 32)
        .ok_or(MaterialItemError::ParameterBounds)?;
    let mut result = Vec::new();
    result
        .try_reserve_exact(count - first)
        .map_err(|_| MaterialItemError::Capture(MaterialCaptureError::Allocation))?;
    for (index, record) in records.as_chunks::<32>().0.iter().enumerate().skip(first) {
        if word(record, 8)? & (1u32 << map) == 0 {
            break;
        }
        let object = word(record, 4)?;
        let mut source = TextureObjectSource {
            record_index: index,
            record: *record,
            target_word: None,
            status: None,
            descriptor: None,
        };
        if record[12] > 2 && object != 0 {
            source.target_word = Some(word(chunk, word(record, 16)? as usize)?);
            let status = object
                .checked_add(16)
                .ok_or(MaterialItemError::ParameterBounds)?;
            let descriptor = object
                .checked_add(48)
                .ok_or(MaterialItemError::ParameterBounds)?;
            source.status = Some(
                reader
                    .copy(status, 16)
                    .map_err(MaterialItemError::Capture)?
                    .try_into()
                    .map_err(|_| MaterialItemError::ParameterBounds)?,
            );
            source.descriptor = Some(
                reader
                    .copy(descriptor, 32)
                    .map_err(MaterialItemError::Capture)?
                    .try_into()
                    .map_err(|_| MaterialItemError::ParameterBounds)?,
            );
        }
        result.push(source);
    }
    Ok(result)
}

/// Copy actual early material sources through the same synchronous source read
/// window as the cohort. This owns every ordinal, including failures and missing
/// roles. It neither changes the candidate population nor admits a native draw.
pub fn copy_main_materials(
    cohort: &OwnedVisibilityCohort,
    resources: Option<&OwnedCohortResources>,
    pass: MaterialPassSources,
    budget: MaterialCopyBudget,
    read: &mut impl FnMut(StreamRequest) -> Result<Vec<u8>, Error>,
) -> Result<OwnedMainMaterials, MaterialCaptureError> {
    copy_main_materials_with_textures(cohort, resources, pass, budget, read, &mut |_| {
        Err(textures::TextureError::MissingSource)
    })
}

pub fn copy_main_materials_with_textures(
    cohort: &OwnedVisibilityCohort,
    resources: Option<&OwnedCohortResources>,
    pass: MaterialPassSources,
    budget: MaterialCopyBudget,
    read: &mut impl FnMut(StreamRequest) -> Result<Vec<u8>, Error>,
    located_read: &mut impl FnMut(
        textures::LocatedTextureRequest,
    ) -> Result<Vec<u8>, textures::TextureError>,
) -> Result<OwnedMainMaterials, MaterialCaptureError> {
    copy_main_materials_with_setup(cohort, resources, pass, budget, None, read, located_read)
}
pub fn copy_main_materials_with_setup(
    cohort: &OwnedVisibilityCohort,
    resources: Option<&OwnedCohortResources>,
    pass: MaterialPassSources,
    budget: MaterialCopyBudget,
    source_user_address: Option<u32>,
    read: &mut impl FnMut(StreamRequest) -> Result<Vec<u8>, Error>,
    located_read: &mut impl FnMut(
        textures::LocatedTextureRequest,
    ) -> Result<Vec<u8>, textures::TextureError>,
) -> Result<OwnedMainMaterials, MaterialCaptureError> {
    copy_main_materials_with_pass_input(
        cohort,
        resources,
        MaterialPassInput {
            parameters: pass,
            snapshot: None,
        },
        budget,
        source_user_address,
        read,
        located_read,
    )
}
/// Borrow the same qualified pass record as geometry. No guest scalar read is
/// synthesized; the original immutable source fields keep their actual values.
pub fn copy_main_materials_with_pass_input(
    cohort: &OwnedVisibilityCohort,
    resources: Option<&OwnedCohortResources>,
    input: MaterialPassInput<'_>,
    budget: MaterialCopyBudget,
    source_user_address: Option<u32>,
    read: &mut impl FnMut(StreamRequest) -> Result<Vec<u8>, Error>,
    located_read: &mut impl FnMut(
        textures::LocatedTextureRequest,
    ) -> Result<Vec<u8>, textures::TextureError>,
) -> Result<OwnedMainMaterials, MaterialCaptureError> {
    let pass = input.parameters;
    if input
        .snapshot
        .is_some_and(|s| s.address() != pass.source_pass_address)
    {
        return Err(MaterialCaptureError::InvalidAssociation);
    }
    let header = cohort.header();
    if let Some(r) = resources
        && (r.vm != header.vm
            || r.source_invocation != header.source_invocation
            || r.execution_update != header.execution_update
            || r.pass_occurrence != header.pass_occurrence
            || r.items.len() != cohort.items().len()
            || r.items
                .iter()
                .enumerate()
                .any(|(i, item)| item.ordinal != i))
    {
        return Err(MaterialCaptureError::InvalidAssociation);
    }
    if pass.source_pass_address == 0 || !pass.source_pass_address.is_multiple_of(4) {
        return Err(MaterialCaptureError::InvalidPass);
    }
    if cohort.items().len() > budget.items {
        return Err(MaterialCaptureError::Budget);
    }
    let mut reader = Reader {
        read,
        pass_snapshot: input.snapshot,
        bytes: 0,
        requested_bytes: 0,
        reads: 0,
        budget,
    };
    if reader.field(pass.source_pass_address, 0xdc)? as i32 != pass.render_mode
        || reader.field(pass.source_pass_address, 0x194)? != pass.technique_override_mask
        || reader.field(pass.source_pass_address, 0x284)? != pass.asset_bank
    {
        return Err(MaterialCaptureError::InvalidPass);
    }
    let override_mode = reader.field(pass.source_pass_address, 0x190)?;
    let binding_payload_address = reader.field(pass.source_pass_address, 0x1fc)?;
    // The pass object's own constructor stores the pass address here; a later
    // or different producer must supply its own source contract before use.
    if binding_payload_address != pass.source_pass_address {
        return Err(MaterialCaptureError::InvalidPass);
    }
    let source_pass_control = reader.field(pass.source_pass_address, 0x144)?;
    // Copy the actual job user once. Setup and Finish own their semantic fields
    // from the same bytes; neither invents a publication or writer lease.
    let user_source = source_user_address.map(|address| reader.copy(address, 160));
    let finish_sources = source_user_address
        .ok_or(finish::FinishCaptureError::MissingSource)
        .and_then(|address| {
            use finish::{FinishCaptureError as E, SourceFinishWords};
            let user = user_source
                .as_ref()
                .ok_or(E::MissingSource)?
                .as_ref()
                .map_err(|e| E::Capture(e.clone()))?;
            let shared = reader
                .copy(
                    pass.source_pass_address
                        .checked_add(0x130)
                        .ok_or(E::Capture(MaterialCaptureError::InvalidPass))?,
                    16,
                )
                .map_err(E::Capture)?;
            let word = |at: usize| u32::from_be_bytes(shared[at..at + 4].try_into().unwrap());
            let source_words = SourceFinishWords {
                reference_increment_address: word(0),
                reference_exchange_address: word(4),
                auxiliary_table_address: word(8),
                auxiliary_table_count: word(12),
                pass_flags: reader
                    .field(pass.source_pass_address, 0x1d0)
                    .map_err(E::Capture)?,
            };
            let mut finish = finish::OwnedMainFinish::decode(
                address,
                pass.source_pass_address,
                user,
                source_words,
            )
            .map_err(E::Decode)?;
            // Opt-in only: no extra read without PS3_RENDER_MAIN_AUX_FLUSH=1.
            if finish::auxiliary_flush_enabled() && finish.lists_enabled {
                let count = source_words.auxiliary_table_count;
                if count > finish::AUXILIARY_RECORD_LIMIT {
                    return Err(E::Decode(finish::FinishError::AuxiliaryTable));
                }
                let records = if count == 0 {
                    Vec::new()
                } else {
                    let table = reader
                        .copy(
                            source_words.auxiliary_table_address,
                            (count as usize * 8).next_multiple_of(16),
                        )
                        .map_err(E::Capture)?;
                    finish::decode_auxiliary_records(&table, count).map_err(E::Decode)?
                };
                finish.auxiliary_records = Some(records.into());
            }
            Ok(finish)
        });
    let setup_sources = source_user_address
        .ok_or(pipeline::MainSetupError::MissingSource)
        .and_then(|_| {
            use pipeline::MainSetupError;
            let user = user_source
                .as_ref()
                .ok_or(MainSetupError::MissingSource)?
                .as_ref()
                .map_err(|e| MainSetupError::Capture(e.clone()))?;
            let stencil = reader
                .field(pass.source_pass_address, 0x114)
                .map_err(MainSetupError::Capture)?
                >> 24
                != 0;
            let packer = reader
                .field(pass.source_pass_address, 0x220)
                .map_err(MainSetupError::Capture)?
                != 0;
            let water = reader
                .copy(
                    pass.source_pass_address
                        .checked_add(0x19c)
                        .ok_or(MainSetupError::Decode(pipeline::PipelineError::Bounds))?,
                    32,
                )
                .map_err(MainSetupError::Capture)?;
            let water = std::array::from_fn(|r| {
                std::array::from_fn(|c| {
                    u32::from_be_bytes(
                        water[r * 16 + c * 4..r * 16 + c * 4 + 4]
                            .try_into()
                            .unwrap(),
                    )
                })
            });
            let defaults = [
                reader
                    .field(pass.source_pass_address, 0x40)
                    .map_err(MainSetupError::Capture)?,
                reader
                    .field(pass.source_pass_address, 0x44)
                    .map_err(MainSetupError::Capture)?,
                reader
                    .field(pass.source_pass_address, 0x48)
                    .map_err(MainSetupError::Capture)?,
            ];
            pipeline::MainPipelineSources::decode(user, stencil, packer, water, defaults)
                .map_err(MainSetupError::Decode)
        });
    let group_twelve_sources = match &setup_sources {
        Ok(s) => group_twelve::copy_group_twelve_textures(
            pass.source_pass_address,
            s.lists_enabled,
            &mut |r| reader.copy(r.address, r.bytes),
        ),
        Err(_) => Err(group_twelve::GroupTwelveError::MissingSource),
    };
    let retained_setup_banks = setup_sources
        .as_ref()
        .ok()
        .filter(|s| s.lists_enabled)
        .and_then(|s| {
            // The behaviour crate copies the eight banks (`material-setup.md`)
            // with its own word and branch budgets; every request still goes
            // through the material reader's charged located copy.
            let mut read = |request| reader.copy_located(request, located_read);
            let memory = MainLocated(std::cell::RefCell::new(&mut read));
            setup::copy_main_setup_banks(
                &memory,
                &setup::MainSetupRecord {
                    enabled_group_mask: s.enabled_groups,
                    lists_enabled: s.lists_enabled,
                    depth_control: s.depth_control,
                    initial: s.setup_references,
                    group_thirteen: s.group13_reference,
                    group_fifteen: s.group15_references,
                    auxiliary: s.auxiliary_reference,
                    restore: s.restore_reference,
                },
            )
        });
    let mut fallback_descriptors = std::array::from_fn(|_| None);
    for (i, target) in fallback_descriptors.iter_mut().enumerate() {
        let address = reader.field(pass.source_pass_address, 0xec + 4 * i as u32)?;
        *target = reader.descriptor(address, 32)?;
    }
    let mut producer_texture_sources = std::array::from_fn(|_| None);
    for (i, target) in producer_texture_sources.iter_mut().enumerate() {
        let address = reader.field(pass.source_pass_address, 0x104 + 4 * i as u32)?;
        *target = reader.descriptor(address, if i == 3 { 48 } else { 32 })?;
    }
    let mut items = Vec::new();
    let mut builtin_pass_values = BuiltinPassValues::default();
    let mut remaining_decoded_bytes = budget.bytes;
    let mut image_cache = textures::TextureImageCache::default();
    // Capture-local exact-input memos for two pure per-item decodes; items
    // repeating a technique/fragment reuse the identical decoded result.
    let mut program_memo = crate::capture_memo::ExactMemo::default();
    let mut layout_memo = crate::capture_memo::ExactMemo::default();
    items
        .try_reserve_exact(cohort.items().len())
        .map_err(|_| MaterialCaptureError::Allocation)?;
    for (ordinal, item) in cohort.items().iter().enumerate() {
        let captured = (|| {
            if override_mode != 0 {
                return Err(MaterialItemError::UnqualifiedOverride(override_mode));
            }
            let existing = resources.and_then(|r| r.items.get(ordinal));
            let owned_material;
            let material = if let Some(material) =
                existing.and_then(|r| r.primary_material.as_ref())
            {
                material
            } else {
                let address = u32::from_be_bytes([item.mbi[6], item.mbi[7], item.mbi[8], 0]);
                let length = ((word(&item.mbi, 28)? & 511) * 16) as usize;
                let bytes = reader
                    .copy(address, length)
                    .map_err(MaterialItemError::Capture)?;
                let snapshot = GuestSnapshot::new(address, &bytes)
                    .map_err(|error| MaterialItemError::Material(SourceError::Snapshot(error)))?;
                owned_material = MaterialSource::copy(&snapshot, address, length, length)
                    .map_err(MaterialItemError::Material)?;
                &owned_material
            };
            let owned_mesh;
            let mesh = if let Some(mesh) = existing.and_then(|r| r.mesh.as_ref()) {
                mesh
            } else {
                let address = word(&item.mbi, 12)?.wrapping_mul(16);
                let bytes = reader
                    .copy(address, 192)
                    .map_err(MaterialItemError::Capture)?;
                let snapshot = GuestSnapshot::new(address, &bytes)
                    .map_err(|error| MaterialItemError::Mesh(MeshSourceError::Snapshot(error)))?;
                owned_mesh =
                    MeshSource::copy(&snapshot, address).map_err(MaterialItemError::Mesh)?;
                &owned_mesh
            };
            let group = mesh.material_group();
            let technique = material
                .select_technique(
                    item.mbi[4],
                    group,
                    pass.render_mode,
                    pass.technique_override_mask,
                    TechniqueResourceBase::Primary,
                )
                .map_err(MaterialItemError::Material)?;
            let owned_chunk;
            let chunk = if let Some(c) = existing
                .and_then(|r| r.primary_technique.as_ref())
                .filter(|c| c.source == technique)
            {
                &c.bytes
            } else {
                owned_chunk = reader
                    .copy(technique.source_address, technique.transfer_bytes as usize)
                    .map_err(MaterialItemError::Capture)?;
                &owned_chunk
            };
            let fragment_limit = u32::try_from(budget.bytes).unwrap_or(u32::MAX);
            let programs = program_memo
                .get_or_decode(chunk, &[], u64::from(fragment_limit), || {
                    decode_technique_programs(
                        chunk,
                        ProgramBudget {
                            source_bytes: chunk.len(),
                            vertex_instructions: chunk.len() / 16,
                            static_constant_rows: chunk.len() / 16,
                            fragment_bytes: fragment_limit,
                        },
                    )
                })
                .map_err(MaterialItemError::Program)?;
            let FragmentSourceReference::AssetBankOffset(offset) = programs.fragment.reference
            else {
                return Err(MaterialItemError::UnqualifiedFragmentReference);
            };
            let fragment_program_address = pass
                .asset_bank
                .checked_add(offset)
                .ok_or(MaterialItemError::ParameterBounds)?;
            let fragment_template = reader
                .copy(
                    fragment_program_address,
                    programs.fragment.transfer_bytes as usize,
                )
                .map_err(MaterialItemError::Capture)?;
            let semantic = &fragment_template[..programs.fragment.semantic_bytes as usize];
            let directory = &programs.fragment.relocation_directory;
            let fragment_seed_layout = layout_memo.get_or_decode(
                semantic,
                directory,
                fragment_template.len() as u64,
                || {
                    FragmentConstantLayout::decode(
                        semantic,
                        directory,
                        0,
                        ConstantBudget {
                            program_bytes: fragment_template.len(),
                            inline_rows: fragment_template.len() / 16,
                            logical_slots: 0,
                            relocations: 0,
                        },
                    )
                },
            );
            let textures =
                texture_sources(material.bytes(), technique.selector, chunk, &mut reader)?;
            let parameters =
                decode_ordinary_parameters(material.bytes(), technique.selector, chunk);
            let ordinary_fragment_rows = parameters
                .as_ref()
                .map_err(Clone::clone)
                .and_then(|parameters| ordinary_fragment_rows(parameters));
            let current_attribute_updates = parameters
                .as_ref()
                .map_err(Clone::clone)
                .and_then(|parameters| current_attribute_updates(parameters));
            let builtin_requests = decode_fragment_builtin_requests(
                material.bytes(),
                material.bytes(),
                technique.selector,
                chunk,
            );
            if let Ok(requests) = &builtin_requests {
                if builtin_pass_values.matrix_words.is_none()
                    && requests
                        .iter()
                        .any(|r| matches!(r, FragmentBuiltinRequest::Matrix4 { .. }))
                {
                    let matrix = reader
                        .copy(binding_payload_address, 64)
                        .map_err(MaterialItemError::Capture)?;
                    builtin_pass_values.matrix_words = Some(std::array::from_fn(|i| {
                        u32::from_be_bytes(matrix[i * 4..i * 4 + 4].try_into().unwrap())
                    }));
                }
                if builtin_pass_values.animation_time.is_none()
                    && requests
                        .iter()
                        .any(|r| matches!(r, FragmentBuiltinRequest::Animated { .. }))
                {
                    builtin_pass_values.animation_time = Some(
                        reader
                            .field(binding_payload_address, 0x1e0)
                            .map_err(MaterialItemError::Capture)?,
                    );
                }
                if builtin_pass_values.alpha_modulation.is_none()
                    && requests
                        .iter()
                        .any(|r| matches!(r, FragmentBuiltinRequest::AlphaTransfer { .. }))
                {
                    builtin_pass_values.alpha_modulation = Some(
                        reader
                            .field(binding_payload_address, 0xd8)
                            .map_err(MaterialItemError::Capture)?,
                    );
                }
            }
            let fragment_builtin_rows = builtin_requests
                .as_ref()
                .map_err(Clone::clone)
                .and_then(|requests| prepare_fragment_builtin_rows(requests, builtin_pass_values));
            // The override guard above and primary source/chunk selection qualify
            // selected == primary; null-object secondary fallback is not used.
            let texture_bindings = textures::copy_primary_texture_bindings_from(
                &textures,
                &fallback_descriptors,
                &mut remaining_decoded_bytes,
                &mut image_cache,
                &mut ReaderSource {
                    reader: &mut reader,
                    located: located_read,
                },
            );
            let draw_sources = draw_sources(
                material.bytes(),
                technique,
                mesh,
                item,
                pass.source_pass_address,
                source_pass_control,
                &mut reader,
            );
            let primary_state = PrimaryStateInput::from_material(material.bytes())
                .map_err(MaterialItemError::State)?;
            let producer_texture_bindings = draw_sources
                .as_ref()
                .map_err(|_| ProducerTextureError::MissingDrawSource)
                .and_then(|draw| {
                    producer_texture_bindings(
                        ProducerTextureInputs {
                            material_flags: primary_state.flags,
                            binding_mask: draw.technique[4],
                            mbi: &item.mbi,
                            pass: pass.source_pass_address,
                            parameters: &producer_texture_sources,
                        },
                        &mut reader,
                        &mut remaining_decoded_bytes,
                        &mut image_cache,
                        located_read,
                    )
                });
            let geometry_attribute_six =
                geometry_attribute_six(mesh, primary_state.flags, &item.mbi, &mut reader);
            Ok(OwnedMaterialSources {
                material_address: material.source_address(),
                primary_state,
                technique,
                programs,
                fragment_program_address,
                fragment_template,
                fragment_seed_layout,
                parameters,
                ordinary_fragment_rows,
                current_attribute_updates,
                geometry_attribute_six,
                builtin_requests,
                fragment_builtin_rows,
                textures,
                texture_bindings,
                producer_texture_bindings,
                draw_sources,
            })
        })();
        items.push(MaterialItemCapture {
            ordinal,
            sources: captured,
        });
    }
    Ok(OwnedMainMaterials {
        pass,
        source_pass_control,
        finish_sources,
        group_twelve_sources,
        setup_sources,
        retained_setup_banks,
        override_mode,
        binding_payload_address,
        fallback_descriptors,
        producer_texture_sources,
        builtin_pass_values,
        items,
        source_bytes: reader.bytes,
        source_reads: reader.reads,
    })
}
