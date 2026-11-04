//! Depth's selected material owns its program resource; primary geometry and
//! alpha inputs keep their independent role. This is a source publication, not
//! native draw admission, a patched shader or an original child output.
use crate::{
    depth_sources::{DepthError, DepthItem, DepthItemRoute},
    material_constants::{BuiltinError, FragmentBuiltinRequest, decode_fragment_builtin_requests},
    material_programs::{
        FragmentSourceReference, ProgramBudget, ProgramError, TechniquePrograms,
        decode_technique_programs,
    },
    material_sources::{
        SourceError, TechniqueResourceBase, TechniqueSource, select_technique_from_bytes,
    },
    shared_scene_sources::SourceContent,
};
use std::{collections::HashMap, ops::Range, sync::Arc};

#[derive(Clone, Debug, PartialEq, Eq)]
pub enum DepthMaterialError {
    Bounds,
    Capture(DepthError),
    OverrideExtent,
    MissingPrimary,
    Selection(SourceError),
    TechniqueExtent,
    Program(ProgramError),
    FragmentReference,
    Budget,
}
#[derive(Debug)]
pub struct DepthOverrideAsset {
    pub material_address: u32,
    pub resource_address: u32,
    pub material: Arc<SourceContent>,
    pub resource: Arc<SourceContent>,
    /// Transfer padding is owned but cannot enlarge a semantic resource.
    pub resource_bytes: usize,
}
#[derive(Debug)]
pub struct DepthProgramVariant {
    /// Whole owned resource or directly acquired primary technique.
    pub resource: Arc<SourceContent>,
    pub range: Range<usize>,
    pub programs: TechniquePrograms,
    pub fragment_address: u32,
    pub fragment_template: Arc<SourceContent>,
}
impl DepthProgramVariant {
    pub fn technique_bytes(&self) -> &[u8] {
        &self.resource.bytes()[self.range.clone()]
    }
    fn decoded_bytes(&self) -> usize {
        std::mem::size_of::<Self>()
            + self.programs.vertex.instructions.capacity() * 16
            + self.programs.vertex.static_constants.capacity()
                * std::mem::size_of::<crate::material_programs::StaticVertexConstant>()
            + self.programs.fragment.relocation_directory.capacity()
    }
}
#[derive(Debug)]
pub struct OwnedDepthMaterial {
    pub primary_address: u32,
    pub primary: Arc<SourceContent>,
    pub selected_address: u32,
    pub selected: Arc<SourceContent>,
    pub override_slot: Option<u8>,
    pub mesh_group: u16,
    pub technique: TechniqueSource,
    pub program: Arc<DepthProgramVariant>,
    /// Selected builtin declarations read PRIMARY alpha. Other binder paths
    /// (including secondary ordinary parameters/textures) are not fabricated.
    pub builtins: Result<Vec<FragmentBuiltinRequest>, BuiltinError>,
}
#[derive(Debug)]
pub struct OwnedDepthMaterials {
    pub overrides: Vec<DepthOverrideAsset>,
    /// Exactly one entry per source ordinal. None is an already classified
    /// repeated/terminating tail, never a failed drawable's replacement.
    pub items: Vec<Option<Result<OwnedDepthMaterial, DepthMaterialError>>>,
    pub decoded_bytes: usize,
}
fn word(b: &[u8], at: usize) -> Result<u32, DepthMaterialError> {
    Ok(u32::from_be_bytes(
        b.get(at..at + 4)
            .ok_or(DepthMaterialError::Bounds)?
            .try_into()
            .map_err(|_| DepthMaterialError::Bounds)?,
    ))
}
fn half(b: &[u8], at: usize) -> Result<u16, DepthMaterialError> {
    Ok(u16::from_be_bytes(
        b.get(at..at + 2)
            .ok_or(DepthMaterialError::Bounds)?
            .try_into()
            .map_err(|_| DepthMaterialError::Bounds)?,
    ))
}
fn acquire(
    read: &mut impl FnMut(u32, usize) -> Result<Arc<SourceContent>, DepthError>,
    address: u32,
    bytes: usize,
) -> Result<Arc<SourceContent>, DepthMaterialError> {
    if address == 0
        || bytes == 0
        || u64::from(address)
            .checked_add(bytes as u64)
            .is_none_or(|end| end > 1u64 << 32)
    {
        return Err(DepthMaterialError::Capture(DepthError::Range));
    }
    let content = read(address, bytes).map_err(DepthMaterialError::Capture)?;
    if content.bytes().len() != bytes {
        return Err(DepthMaterialError::Capture(DepthError::Read));
    }
    Ok(content)
}
/// The depth pass's override slot selection. Group7 branches on PRIMARY flags,
/// and group5/flag24 force the unskinned category1 even for a styled item.
/// The caller still separately classifies productive source groups.
pub fn depth_override_slot(group: u16, primary_flags: u32, mbi_style: u8) -> u8 {
    let row = u8::from(mbi_style & 62 != 0) * 4;
    match group {
        5 => 1,
        7 if primary_flags & (1 << 24) != 0 => 1,
        2 | 4 | 11 => row + 2,
        7 if primary_flags & (1 << 23) != 0 => row + 2,
        3 => row + 3,
        _ => row,
    }
}
/// Acquire the real eight startup override records once, then borrow selected
/// technique subspans. Shared program keys use exact catalog content identities
/// and ranges, not guest EA, mesh identity or an execution-update shortcut.
/// Every fresh callback remains subject to the caller's source-window budget.
pub fn copy_depth_materials(
    pass: &[u8],
    items: &[DepthItem],
    decoded_budget: usize,
    read: &mut impl FnMut(u32, usize) -> Result<Arc<SourceContent>, DepthError>,
) -> Result<OwnedDepthMaterials, DepthMaterialError> {
    let override_enabled = word(pass, 0x190)? != 0;
    let render_mode = word(pass, 0xdc)? as i32;
    let mask = word(pass, 0x194)?;
    let bank = word(pass, 0x284)?;
    let mut overrides = Vec::new();
    if override_enabled {
        for slot in 0..8 {
            let bytes = word(pass, 0x150 + 8 * slot)? as usize;
            let address = word(pass, 0x154 + 8 * slot)?;
            if address == 0 || bytes < 0x60 || !bytes.is_multiple_of(16) {
                return Err(DepthMaterialError::OverrideExtent);
            }
            let material = acquire(read, address, bytes)?;
            let resource_address = word(material.bytes(), 0x38)?;
            let declared = usize::from(half(material.bytes(), 0x40)?);
            if resource_address == 0 || declared == 0 {
                return Err(DepthMaterialError::OverrideExtent);
            }
            let transfer = declared
                .checked_add(15)
                .ok_or(DepthMaterialError::OverrideExtent)?
                & !15;
            let resource = acquire(read, resource_address, transfer)?;
            overrides.push(DepthOverrideAsset {
                material_address: address,
                resource_address,
                material,
                resource,
                resource_bytes: declared,
            });
        }
    }
    let mut decoded_bytes = std::mem::size_of::<OwnedDepthMaterials>()
        + overrides.capacity() * std::mem::size_of::<DepthOverrideAsset>()
        + items.len()
            * std::mem::size_of::<Option<Result<OwnedDepthMaterial, DepthMaterialError>>>();
    if decoded_bytes > decoded_budget {
        return Err(DepthMaterialError::Budget);
    }
    let mut programs = HashMap::<(u64, usize, usize, u32), Arc<DepthProgramVariant>>::new();
    let mut result = Vec::new();
    result
        .try_reserve_exact(items.len())
        .map_err(|_| DepthMaterialError::Budget)?;
    for item in items {
        if item.route != DepthItemRoute::Prepare {
            result.push(None);
            continue;
        }
        let captured = (|| {
            let primary = item
                .material
                .as_ref()
                .ok_or(DepthMaterialError::MissingPrimary)?;
            let mesh = item
                .mesh
                .as_ref()
                .ok_or(DepthMaterialError::MissingPrimary)?;
            let group = half(mesh.bytes(), 0xbc)?;
            let flags = word(primary.bytes(), 0x58)?;
            let slot = override_enabled.then(|| depth_override_slot(group, flags, item.mbi[30]));
            let asset = slot.map(|slot| &overrides[usize::from(slot)]);
            let selected = asset.map_or(primary, |a| &a.material);
            let selected_address = asset.map_or(
                u32::from_be_bytes([item.mbi[6], item.mbi[7], item.mbi[8], 0]),
                |a| a.material_address,
            );
            let technique = select_technique_from_bytes(
                selected.bytes(),
                item.mbi[4],
                group,
                render_mode,
                mask,
                asset.map_or(TechniqueResourceBase::Primary, |a| {
                    TechniqueResourceBase::Override(a.resource_address)
                }),
            )
            .map_err(DepthMaterialError::Selection)?;
            if technique.declared_bytes == 0 {
                return Err(DepthMaterialError::TechniqueExtent);
            }
            let (resource, range) = if let Some(asset) = asset {
                let low = technique.relative_offset as usize;
                let high = low
                    .checked_add(technique.transfer_bytes as usize)
                    .filter(|high| *high <= asset.resource.bytes().len())
                    .ok_or(DepthMaterialError::TechniqueExtent)?;
                if low
                    .checked_add(technique.declared_bytes as usize)
                    .is_none_or(|end| end > asset.resource_bytes)
                {
                    return Err(DepthMaterialError::TechniqueExtent);
                }
                (Arc::clone(&asset.resource), low..high)
            } else {
                let resource = acquire(
                    read,
                    technique.source_address,
                    technique.transfer_bytes as usize,
                )?;
                (resource, 0..technique.transfer_bytes as usize)
            };
            let key = (resource.identity, range.start, range.end, bank);
            let program = if let Some(program) = programs.get(&key) {
                Arc::clone(program)
            } else {
                let chunk = &resource.bytes()[range.clone()];
                let programs_decoded = decode_technique_programs(
                    chunk,
                    ProgramBudget {
                        source_bytes: chunk.len(),
                        vertex_instructions: chunk.len() / 16,
                        static_constant_rows: chunk.len() / 16,
                        fragment_bytes: u32::try_from(decoded_budget).unwrap_or(u32::MAX),
                    },
                )
                .map_err(DepthMaterialError::Program)?;
                let FragmentSourceReference::AssetBankOffset(offset) =
                    programs_decoded.fragment.reference
                else {
                    return Err(DepthMaterialError::FragmentReference);
                };
                let fragment_address = bank
                    .checked_add(offset)
                    .ok_or(DepthMaterialError::FragmentReference)?;
                let fragment_template = acquire(
                    read,
                    fragment_address,
                    programs_decoded.fragment.transfer_bytes as usize,
                )?;
                let program = Arc::new(DepthProgramVariant {
                    resource,
                    range,
                    programs: programs_decoded,
                    fragment_address,
                    fragment_template,
                });
                decoded_bytes = decoded_bytes
                    .checked_add(program.decoded_bytes())
                    .filter(|n| *n <= decoded_budget)
                    .ok_or(DepthMaterialError::Budget)?;
                programs.insert(key, Arc::clone(&program));
                program
            };
            let builtins = decode_fragment_builtin_requests(
                selected.bytes(),
                primary.bytes(),
                technique.selector,
                program.technique_bytes(),
            );
            if let Ok(requests) = &builtins {
                decoded_bytes = decoded_bytes
                    .checked_add(
                        requests.capacity() * std::mem::size_of::<FragmentBuiltinRequest>(),
                    )
                    .filter(|n| *n <= decoded_budget)
                    .ok_or(DepthMaterialError::Budget)?;
            }
            Ok(OwnedDepthMaterial {
                primary_address: u32::from_be_bytes([item.mbi[6], item.mbi[7], item.mbi[8], 0]),
                primary: Arc::clone(primary),
                selected_address,
                selected: Arc::clone(selected),
                override_slot: slot,
                mesh_group: group,
                technique,
                program,
                builtins,
            })
        })();
        result.push(Some(captured));
    }
    Ok(OwnedDepthMaterials {
        overrides,
        items: result,
        decoded_bytes,
    })
}
