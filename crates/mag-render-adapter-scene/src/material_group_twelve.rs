//! The Main pass's group-twelve compact texture unit inputs, copied before
//! Main runs.
//! These are source descriptors only; generated GPU contents need native leases.
use crate::partition_inputs::StreamRequest;
use crate::{material_capture::MaterialCaptureError, material_textures::OwnedDescriptorSource};
#[derive(Clone, Debug, PartialEq, Eq)]
pub enum GroupTwelveError {
    MissingSource,
    Bounds,
    Extent,
    Source(MaterialCaptureError),
}
#[derive(Clone, Debug, PartialEq, Eq)]
pub struct OwnedGroupTwelveTextures {
    pub pass_address: u32,
    /// Actual pass1BC/1C0 -> unit12/unit13 descriptors. Unit11 is the existing
    /// full pass110 source, retained separately in producer_texture_sources[3].
    pub units: [OwnedDescriptorSource; 2],
}
pub fn copy_group_twelve_textures(
    pass_address: u32,
    enabled: bool,
    read: &mut impl FnMut(StreamRequest) -> Result<Vec<u8>, MaterialCaptureError>,
) -> Result<Option<OwnedGroupTwelveTextures>, GroupTwelveError> {
    if !enabled {
        return Ok(None);
    }
    if pass_address == 0
        || !pass_address.is_multiple_of(4)
        || pass_address.checked_add(0x1c4).is_none()
    {
        return Err(GroupTwelveError::Bounds);
    }
    let mut get = |address: u32, bytes: usize| -> Result<Vec<u8>, GroupTwelveError> {
        if address == 0
            || !address.is_multiple_of(4)
            || u64::from(address) + bytes as u64 > 1u64 << 32
        {
            return Err(GroupTwelveError::Bounds);
        }
        let b = read(StreamRequest { address, bytes }).map_err(GroupTwelveError::Source)?;
        if b.len() != bytes {
            return Err(GroupTwelveError::Extent);
        }
        Ok(b)
    };
    let mut units = Vec::with_capacity(2);
    for at in [0x1bc, 0x1c0] {
        let pointer = get(pass_address + at, 4)?;
        let address = u32::from_be_bytes(pointer.try_into().map_err(|_| GroupTwelveError::Extent)?);
        let bytes = get(address, 32)?;
        units.push(OwnedDescriptorSource { address, bytes });
    }
    Ok(Some(OwnedGroupTwelveTextures {
        pass_address,
        units: units.try_into().map_err(|_| GroupTwelveError::Extent)?,
    }))
}
