//! Raw asset resolution from an explicitly retained visibility item.
//!
//! These owned records prove the consistency of supplied snapshot bytes only.
//! They do not establish streaming ownership, a source publication, full draw
//! state, or eligibility to suppress any original producer.
use crate::{error::Error, guest_memory::GuestSnapshot};
use std::sync::Arc;

#[derive(Clone, Debug, PartialEq, Eq)]
pub enum SourceError {
    InvalidCamera,
    MissingCameraTransform,
    NullResource,
    BudgetExceeded,
    MaterialBounds,
    Snapshot(Error),
}
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub struct SelectedResourceAddresses {
    pub material_address: u32,
    pub material_bytes: usize,
    pub mesh_address: u32,
    pub transform_address: u32,
    pub mesh_group: u8,
}
fn word(bytes: &[u8], offset: usize) -> Result<u32, SourceError> {
    let source = bytes
        .get(offset..offset + 4)
        .ok_or(SourceError::MaterialBounds)?;
    Ok(u32::from_be_bytes(
        source.try_into().map_err(|_| SourceError::MaterialBounds)?,
    ))
}
/// Decode source addresses without issuing reads or interpreting the transform.
/// Guest address arithmetic wraps; the eventual source reader still validates
/// the entire resulting range. Missing camera transforms are explicit failures.
pub fn selected_resource_addresses(
    item: &[u8; 32],
    camera: u32,
    transform_base: u32,
) -> Result<SelectedResourceAddresses, SourceError> {
    if camera > 3 {
        return Err(SourceError::InvalidCamera);
    }
    let camera_bit = 1 << (3 - camera);
    if word(item, 20)? & camera_bit == 0 {
        return Err(SourceError::MissingCameraTransform);
    }
    let material_address = u32::from_be_bytes([item[6], item[7], item[8], 0]);
    let material_bytes = ((word(item, 28)? & 511) * 16) as usize;
    let slot = u32::from(u16::from_be_bytes([item[28], item[29]]) & 0x7fff)
        + (word(item, 16)? & 15 & (camera_bit - 1)).count_ones();
    Ok(SelectedResourceAddresses {
        material_address,
        material_bytes,
        mesh_address: word(item, 12)?.wrapping_mul(16),
        transform_address: transform_base.wrapping_add(slot * 112),
        mesh_group: item[0] >> 4,
    })
}
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub enum TechniqueResourceBase {
    Primary,
    /// The material itself must be the selected override material. This is the
    /// independently owned override resource base, not its primary counterpart.
    Override(u32),
}
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub struct TechniqueSource {
    pub selector: u32,
    pub table_index: u8,
    pub resource_base: u32,
    pub relative_offset: u32,
    pub source_address: u32,
    pub declared_bytes: u32,
    pub transfer_bytes: u32,
}
pub struct MaterialSource {
    source_address: u32,
    bytes: MaterialBytes,
}
enum MaterialBytes {
    Owned(Vec<u8>),
    Shared(Arc<crate::shared_scene_sources::SourceContent>),
}
impl MaterialSource {
    pub fn copy(
        snapshot: &GuestSnapshot<'_>,
        address: u32,
        length: usize,
        byte_budget: usize,
    ) -> Result<Self, SourceError> {
        if address == 0 || length == 0 {
            return Err(SourceError::NullResource);
        }
        if length > byte_budget {
            return Err(SourceError::BudgetExceeded);
        }
        let bytes = snapshot
            .copy_bytes(u64::from(address), length)
            .map_err(SourceError::Snapshot)?;
        Self::from_owned(address, bytes, byte_budget)
    }
    /// Consume a range already copied by the actual source reader. This avoids
    /// a second snapshot copy; it neither skips that read nor acquires a lease.
    /// The existing decoded-resource opt-in may share exact immutable bytes.
    /// Catalog refusal always keeps the supplied Vec, without changing results.
    pub fn from_owned(
        address: u32,
        bytes: Vec<u8>,
        byte_budget: usize,
    ) -> Result<Self, SourceError> {
        if address == 0 || bytes.is_empty() {
            return Err(SourceError::NullResource);
        }
        if bytes.len() > byte_budget {
            return Err(SourceError::BudgetExceeded);
        }
        GuestSnapshot::new(address, &bytes).map_err(SourceError::Snapshot)?;
        let shared = crate::decoded_geometry_cache::enabled()
            .then(|| {
                let _timer = crate::source_acquisition::section(
                    crate::source_acquisition::Section::MaterialIntern,
                );
                crate::shared_scene_sources::retain_content(&bytes).ok()
            })
            .flatten();
        Ok(Self {
            source_address: address,
            bytes: shared.map_or_else(|| MaterialBytes::Owned(bytes), MaterialBytes::Shared),
        })
    }
    pub fn source_address(&self) -> u32 {
        self.source_address
    }
    pub fn bytes(&self) -> &[u8] {
        match &self.bytes {
            MaterialBytes::Owned(bytes) => bytes,
            MaterialBytes::Shared(content) => content.bytes(),
        }
    }
    /// Select a complete map record and table entry from the owned material.
    /// Resolving this source is not shader translation or material-state closure.
    pub fn select_technique(
        &self,
        item_selector: u8,
        mesh_material_group: u16,
        render_mode: i32,
        override_mask: u32,
        resource: TechniqueResourceBase,
    ) -> Result<TechniqueSource, SourceError> {
        select_technique_from_bytes(
            self.bytes(),
            item_selector,
            mesh_material_group,
            render_mode,
            override_mask,
            resource,
        )
    }
}
/// The same selector over an already owned immutable source span. Depth keeps
/// its selected override asset Arc instead of cloning its material per item;
/// Main delegates here too. This neither reads guest memory nor grants a lease.
pub fn select_technique_from_bytes(
    material: &[u8],
    item_selector: u8,
    mesh_material_group: u16,
    render_mode: i32,
    override_mask: u32,
    resource: TechniqueResourceBase,
) -> Result<TechniqueSource, SourceError> {
    let initial = if mesh_material_group == 15 {
        item_selector & 7
    } else if render_mode <= 15 {
        item_selector >> 3
    } else {
        0
    };
    let selector = u32::from(initial) & !override_mask;
    let map_base = usize::from(u16::from_be_bytes(
        material
            .get(0x48..0x4a)
            .ok_or(SourceError::MaterialBounds)?
            .try_into()
            .map_err(|_| SourceError::MaterialBounds)?,
    ));
    let map_start = map_base + selector as usize * 32;
    let map = material
        .get(map_start..map_start + 32)
        .ok_or(SourceError::MaterialBounds)?;
    let table_index = map[1];
    let table_base = usize::from(u16::from_be_bytes(
        material
            .get(0x4a..0x4c)
            .ok_or(SourceError::MaterialBounds)?
            .try_into()
            .map_err(|_| SourceError::MaterialBounds)?,
    ));
    let entry_start = table_base + usize::from(table_index) * 8;
    let entry = material
        .get(entry_start..entry_start + 8)
        .ok_or(SourceError::MaterialBounds)?;
    let relative_offset = word(entry, 0)?;
    let declared_bytes = word(entry, 4)?;
    // Reject an extent whose transfer alignment would wrap. No other size
    // limit applies to a native resource.
    let transfer_bytes = declared_bytes
        .checked_add(15)
        .ok_or(SourceError::MaterialBounds)?
        & !15;
    let resource_base = match resource {
        TechniqueResourceBase::Primary => word(material, 0x38)?,
        TechniqueResourceBase::Override(base) => base,
    };
    if resource_base == 0 {
        return Err(SourceError::NullResource);
    }
    Ok(TechniqueSource {
        selector,
        table_index,
        resource_base,
        relative_offset,
        source_address: resource_base.wrapping_add(relative_offset),
        declared_bytes,
        transfer_bytes,
    })
}
