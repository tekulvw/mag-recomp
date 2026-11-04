//! Main group-twelve texture and state setup (`main-group-twelve.md`).

use crate::guest::{GuestMemory, SourceError, be_u32, be_words};
use crate::setup::{Comparison, PipelineState};
use crate::texture::TextureUnitRecord;

/// Pass-object offsets of the three descriptor addresses.
pub const UNIT11_FULL: u32 = 0x110;
pub const UNIT12_COMPACT: u32 = 0x1bc;
pub const UNIT13_COMPACT: u32 = 0x1c0;

#[derive(Clone, Debug, PartialEq, Eq)]
pub enum GroupTwelveError {
    /// `PASS` is zero, misaligned, or `PASS+0x1C4` overflows.
    Pass(u32),
    /// The sources belong to a different pass object than the one being planned.
    ForeignPass {
        pass: u32,
        planned: u32,
    },
    /// A descriptor address is zero or misaligned.
    Source {
        unit: u32,
        address: u32,
    },
    Read(SourceError),
    /// A read returned the wrong number of bytes.
    Length {
        address: u32,
        expected: u32,
        actual: usize,
    },
}

/// One request the caller resolves to an image whose provenance is exactly this pair.
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub struct ImageRequest {
    pub unit: u32,
    pub source_address: u32,
    pub descriptor: [u32; 8],
}

#[derive(Clone, Debug, PartialEq, Eq)]
pub struct GroupTwelveOutput {
    pub units: [TextureUnitRecord; 16],
    pub state: PipelineState,
    /// Units whose previous image association is forgotten.
    pub forgotten_images: [u32; 3],
    pub requests: [ImageRequest; 3],
}

fn read<M: GuestMemory + ?Sized>(
    memory: &M,
    address: u32,
    length: u32,
) -> Result<Vec<u8>, GroupTwelveError> {
    let bytes = memory
        .read(address, length)
        .map_err(GroupTwelveError::Read)?;
    if bytes.len() != length as usize {
        return Err(GroupTwelveError::Length {
            address,
            expected: length,
            actual: bytes.len(),
        });
    }
    Ok(bytes)
}

/// Runs the group-twelve setup. Returns `Ok(None)` when lists are disabled (nothing happens).
pub fn run_group_twelve<M: GuestMemory + ?Sized>(
    memory: &M,
    lists_enabled: bool,
    pass: u32,
    planned_pass: u32,
    units: &[TextureUnitRecord; 16],
    state: &PipelineState,
) -> Result<Option<GroupTwelveOutput>, GroupTwelveError> {
    if !lists_enabled {
        return Ok(None);
    }
    if pass == 0 || !pass.is_multiple_of(4) || pass.checked_add(0x1c4).is_none() {
        return Err(GroupTwelveError::Pass(pass));
    }
    if pass != planned_pass {
        return Err(GroupTwelveError::ForeignPass {
            pass,
            planned: planned_pass,
        });
    }
    let pointer = |unit: u32, offset: u32| -> Result<u32, GroupTwelveError> {
        let address = be_u32(&read(memory, pass + offset, 4)?, 0);
        if address == 0 || !address.is_multiple_of(4) {
            return Err(GroupTwelveError::Source { unit, address });
        }
        Ok(address)
    };
    let full_address = pointer(11, UNIT11_FULL)?;
    let address12 = pointer(12, UNIT12_COMPACT)?;
    let address13 = pointer(13, UNIT13_COMPACT)?;
    let full: [u32; 12] = be_words(&read(memory, full_address, 48)?, 0);
    let compact12: [u32; 8] = be_words(&read(memory, address12, 32)?, 0);
    let compact13: [u32; 8] = be_words(&read(memory, address13, 32)?, 0);

    let mut units = *units;
    units[11].apply_full(&full);
    units[12].apply_compact(&compact12);
    units[13].apply_compact(&compact13);
    units[14].disable_if_known();
    let state = PipelineState {
        colour_masks: Some([15; 4]),
        depth_test: Some(true),
        depth_write: Some(true),
        depth_comparison: Some(Comparison::LessOrEqual),
        blend_enable: Some(false),
        alpha_test: Some(false),
        ..*state
    };
    let requests = [
        ImageRequest {
            unit: 11,
            source_address: full_address,
            descriptor: units[11].compact.expect("unit 11 was just set"),
        },
        ImageRequest {
            unit: 12,
            source_address: address12,
            descriptor: compact12,
        },
        ImageRequest {
            unit: 13,
            source_address: address13,
            descriptor: compact13,
        },
    ];
    Ok(Some(GroupTwelveOutput {
        units,
        state,
        forgotten_images: [11, 12, 13],
        requests,
    }))
}

/// A Main item as the required-unit rule sees it.
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub struct ItemUsage {
    pub group: u32,
    /// The item's route is "prepare".
    pub prepare: bool,
    /// Units its fragment program samples (bit `u` for unit `u`); `None` when the program
    /// cannot be parsed.
    pub sampled_units: Option<u16>,
}

#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub enum RequiredUnitsError {
    /// The fragment program of the item at this position cannot be parsed.
    UnparsedProgram { item: usize },
}

/// The group-twelve units (bits 11, 12, 13) that later items need as importable images.
pub fn required_group_twelve_units(
    lists_enabled: bool,
    items: &[ItemUsage],
) -> Result<u16, RequiredUnitsError> {
    if !lists_enabled {
        return Ok(0);
    }
    const MASK: u16 = (1 << 11) | (1 << 12) | (1 << 13);
    let mut required = 0;
    for (position, item) in items.iter().enumerate() {
        if item.group < 12 || !item.prepare {
            continue;
        }
        let units = item
            .sampled_units
            .ok_or(RequiredUnitsError::UnparsedProgram { item: position })?;
        required |= units & MASK;
    }
    Ok(required)
}

#[cfg(test)]
mod tests {
    use super::*;
    use crate::guest::SparseMemory;

    const PASS: u32 = 0x1_0000;

    fn memory() -> SparseMemory {
        let mut m = SparseMemory::new();
        m.put(PASS, &[0; 0x1c4]);
        m.put_u32(PASS + UNIT11_FULL, 0x3000);
        m.put_u32(PASS + UNIT12_COMPACT, 0x2000);
        m.put_u32(PASS + UNIT13_COMPACT, 0x2040);
        let full: Vec<u32> = (0x10..0x1c).collect();
        m.put_words(0x3000, &full);
        m.put_words(0x2000, &[0x120; 8]);
        m.put_words(0x2040, &[0x130; 8]);
        m
    }

    fn run(
        m: &SparseMemory,
        units: &[TextureUnitRecord; 16],
        state: &PipelineState,
    ) -> Result<Option<GroupTwelveOutput>, GroupTwelveError> {
        run_group_twelve(m, true, PASS, PASS, units, state)
    }

    #[test]
    fn vector_1_full_to_compact() {
        let out = run(&memory(), &Default::default(), &PipelineState::default())
            .unwrap()
            .unwrap();
        let unit = out.units[11];
        assert_eq!(
            unit.compact,
            Some([0x10, 0x11, 0x12, 0x13, 0x14, 0x15, 0x16, 0x18])
        );
        assert_eq!(unit.border, Some(0x17));
        assert_eq!(unit.texcoord_control, Some(0x19));
        assert_eq!(unit.vertex_lod_control, Some(0x1a));
    }

    #[test]
    fn vector_2_unit_fourteen() {
        let mut units: [TextureUnitRecord; 16] = Default::default();
        units[14].compact = Some([0x1000, 0, 0, 0x8000_0000, 0, 0, 0, 0]);
        units[14].border = Some(5);
        let out = run(&memory(), &units, &PipelineState::default())
            .unwrap()
            .unwrap();
        assert_eq!(out.units[14].compact, Some([0x1000, 0, 0, 0, 0, 0, 0, 0]));
        assert_eq!(out.units[14].border, Some(5));
        let out = run(&memory(), &Default::default(), &PipelineState::default())
            .unwrap()
            .unwrap();
        assert_eq!(out.units[14].compact, None);
    }

    #[test]
    fn vector_3_state() {
        let before = PipelineState {
            depth_test: Some(false),
            blend_enable: Some(true),
            colour_masks: Some([8; 4]),
            stencil_enable: Some(true),
            ..Default::default()
        };
        let out = run(&memory(), &Default::default(), &before)
            .unwrap()
            .unwrap();
        let s = out.state;
        assert_eq!(
            (s.depth_test, s.depth_write, s.depth_comparison),
            (Some(true), Some(true), Some(Comparison::LessOrEqual))
        );
        assert_eq!(
            (s.blend_enable, s.alpha_test, s.colour_masks),
            (Some(false), Some(false), Some([15; 4]))
        );
        assert_eq!(s.stencil_enable, Some(true));
    }

    #[test]
    fn vector_4_requests() {
        let out = run(&memory(), &Default::default(), &PipelineState::default())
            .unwrap()
            .unwrap();
        assert_eq!(
            out.requests[1],
            ImageRequest {
                unit: 12,
                source_address: 0x2000,
                descriptor: [0x120; 8]
            }
        );
        assert_eq!(
            out.requests[2],
            ImageRequest {
                unit: 13,
                source_address: 0x2040,
                descriptor: [0x130; 8]
            }
        );
        assert_eq!(out.requests[0].unit, 11);
        assert_eq!(out.requests[0].source_address, 0x3000);
        assert_eq!(out.requests[0].descriptor[7], 0x18);
        assert_eq!(out.forgotten_images, [11, 12, 13]);
    }

    #[test]
    fn vector_5_required_units() {
        let items = [
            ItemUsage {
                group: 3,
                prepare: true,
                sampled_units: Some(1 << 11),
            },
            ItemUsage {
                group: 12,
                prepare: true,
                sampled_units: Some((1 << 0) | (1 << 11)),
            },
            ItemUsage {
                group: 13,
                prepare: true,
                sampled_units: Some((1 << 12) | (1 << 14)),
            },
            ItemUsage {
                group: 14,
                prepare: false,
                sampled_units: Some(1 << 13),
            },
        ];
        assert_eq!(
            required_group_twelve_units(true, &items),
            Ok((1 << 11) | (1 << 12))
        );
        assert_eq!(required_group_twelve_units(false, &items), Ok(0));
    }

    #[test]
    fn vector_6_unparseable_program() {
        let items = [ItemUsage {
            group: 12,
            prepare: true,
            sampled_units: None,
        }];
        assert_eq!(
            required_group_twelve_units(true, &items),
            Err(RequiredUnitsError::UnparsedProgram { item: 0 })
        );
        // Groups below 12 contribute nothing, even unparsed.
        let items = [ItemUsage {
            group: 11,
            prepare: true,
            sampled_units: None,
        }];
        assert_eq!(required_group_twelve_units(true, &items), Ok(0));
    }

    #[test]
    fn vector_7_null_source_and_other_errors() {
        let mut m = memory();
        m.put_u32(PASS + UNIT13_COMPACT, 0);
        assert_eq!(
            run(&m, &Default::default(), &PipelineState::default()),
            Err(GroupTwelveError::Source {
                unit: 13,
                address: 0
            })
        );
        let mut m = memory();
        m.put_u32(PASS + UNIT12_COMPACT, 0x2002);
        assert_eq!(
            run(&m, &Default::default(), &PipelineState::default()),
            Err(GroupTwelveError::Source {
                unit: 12,
                address: 0x2002
            })
        );
        assert_eq!(
            run_group_twelve(
                &memory(),
                true,
                PASS,
                PASS + 0x400,
                &Default::default(),
                &PipelineState::default()
            ),
            Err(GroupTwelveError::ForeignPass {
                pass: PASS,
                planned: PASS + 0x400
            })
        );
        assert_eq!(
            run_group_twelve(
                &memory(),
                true,
                0,
                0,
                &Default::default(),
                &PipelineState::default()
            ),
            Err(GroupTwelveError::Pass(0))
        );
        assert_eq!(
            run_group_twelve(
                &memory(),
                true,
                0xffff_fe40,
                0xffff_fe40,
                &Default::default(),
                &PipelineState::default()
            ),
            Err(GroupTwelveError::Pass(0xffff_fe40))
        );
        // A short descriptor.
        let mut m = memory();
        m.put_u32(PASS + UNIT13_COMPACT, 0x9000);
        m.put_words(0x9000, &[1; 4]);
        assert!(matches!(
            run(&m, &Default::default(), &PipelineState::default()),
            Err(GroupTwelveError::Length {
                address: 0x9000,
                expected: 32,
                actual: 16
            })
        ));
        assert_eq!(
            run_group_twelve(
                &memory(),
                false,
                0,
                1,
                &Default::default(),
                &PipelineState::default()
            ),
            Ok(None)
        );
    }
}
