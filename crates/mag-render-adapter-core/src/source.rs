//! Owned raw inputs for the source-proven Shadow corner helper subset.
//!
//! The fields are the manager scalars and view pose rows that the Shadow corner
//! helper reads. No completed Shadow contexts, corner/center tables, material
//! packets or original worker output supplies these source fields.
//! Runtime writer readiness has not been proved: there is no ready flag or
//! integer certificate that can promote this snapshot into a complete frame.
use crate::{error::Error, guest_memory::GuestSnapshot};
use std::sync::{Arc, Weak};

/// Logical copied source payload; excludes Arc/control-block/bookkeeping bytes.
pub const SHADOW_SOURCE_BYTES: usize = 84;
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
/// Local copy identity only, not guest allocation generation or WorldUpdateId.
pub struct SnapshotId {
    pub scene: u64,
    pub serial: u64,
}
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub struct ShadowSourceAddress {
    pub manager: u32,
    pub view: u32,
}
#[derive(Clone, Debug, PartialEq, Eq)]
/// Bit-preserving owned source words, with no numerical normalization.
pub struct ShadowCornerSources {
    /// Manager source scalar words at +1504/+1508/+150C.
    pub parameters: [u32; 3],
    /// Four source pose rows at view +10..+4F; fourth row is read-only getter data.
    pub pose_rows: [[u32; 4]; 4],
    pub view_scalar_60: u32,
    pub view_scalar_a24: u32,
}
#[derive(Clone, Debug, PartialEq, Eq)]
/// Complete raw fields for one helper; explicitly incomplete whole-frame input.
pub struct PartialShadowSnapshot {
    pub id: SnapshotId,
    pub sources: ShadowCornerSources,
}
impl PartialShadowSnapshot {
    /// No runtime producer/camera publication has been wired or authenticated.
    /// Source copy identity and immutable bytes can never satisfy this check.
    pub fn require_complete_frame_input(&self) -> Result<(), Error> {
        Err(Error::UnprovenProducerEpoch)
    }
}
#[derive(Clone, Copy, Debug, Default, PartialEq, Eq)]
/// Lifetime copy counts only; no upload, GPU, allocation or frame-rate claims.
pub struct CaptureStats {
    pub snapshots: u64,
    pub copied_bytes: u64,
}
/// A bounded source-copy owner. Returned Arcs may outlive this adapter; their
/// actual last reference controls slot reuse, including across local resets.
/// There is no guest mutation, hook suppression, submission or completion path.
pub struct RetailAdapter {
    stats: CaptureStats,
    max_inflight: usize,
    inflight: Vec<Weak<PartialShadowSnapshot>>,
    scene: u64,
    serial: u64,
}
impl RetailAdapter {
    pub fn new(max_inflight_snapshots: usize) -> Result<Self, Error> {
        if max_inflight_snapshots == 0 {
            return Err(Error::InvalidCapacity);
        }
        let mut inflight = Vec::new();
        inflight
            .try_reserve_exact(max_inflight_snapshots)
            .map_err(|_| Error::AllocationFailed)?;
        Ok(Self {
            stats: CaptureStats::default(),
            max_inflight: max_inflight_snapshots,
            inflight,
            scene: 1,
            serial: 0,
        })
    }
    pub fn publish_shadow_snapshot(
        &mut self,
        memory: &GuestSnapshot<'_>,
        address: ShadowSourceAddress,
    ) -> Result<Arc<PartialShadowSnapshot>, Error> {
        self.inflight.retain(|entry| entry.strong_count() != 0);
        if self.inflight.len() == self.max_inflight {
            return Err(Error::CapacityExceeded);
        }
        if address.manager == 0 || address.view == 0 {
            return Err(Error::InvalidSourceAddress);
        }
        let manager = u64::from(address.manager);
        let view = u64::from(address.view);
        // Check every span before copying. No default-zero partial publication.
        for (address, length) in [
            (manager + 0x1504, 12),
            (view + 0x10, 64),
            (view + 0x60, 4),
            (view + 0xa24, 4),
        ] {
            memory.range(address, length)?;
        }
        let serial = self.serial.checked_add(1).ok_or(Error::IdentityExhausted)?;
        let stats = CaptureStats {
            snapshots: self
                .stats
                .snapshots
                .checked_add(1)
                .ok_or(Error::IdentityExhausted)?,
            copied_bytes: self
                .stats
                .copied_bytes
                .checked_add(SHADOW_SOURCE_BYTES as u64)
                .ok_or(Error::IdentityExhausted)?,
        };
        let pose: [u32; 16] = memory.words(view + 0x10)?;
        let snapshot = Arc::new(PartialShadowSnapshot {
            id: SnapshotId {
                scene: self.scene,
                serial,
            },
            sources: ShadowCornerSources {
                parameters: memory.words(manager + 0x1504)?,
                pose_rows: core::array::from_fn(|row| {
                    core::array::from_fn(|lane| pose[row * 4 + lane])
                }),
                view_scalar_60: memory.words::<1>(view + 0x60)?[0],
                view_scalar_a24: memory.words::<1>(view + 0xa24)?[0],
            },
        });
        self.inflight.push(Arc::downgrade(&snapshot));
        self.serial = serial;
        self.stats = stats;
        Ok(snapshot)
    }
    pub fn stats(&self) -> CaptureStats {
        self.stats
    }
    /// Local invalidation boundary only: never asserts a real guest scene epoch.
    pub fn reset_scene(&mut self) -> Result<(), Error> {
        self.scene = self.scene.checked_add(1).ok_or(Error::IdentityExhausted)?;
        Ok(())
    }
}
