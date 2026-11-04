//! Owned retained-visibility records copied at a real source publication.
//!
//! This is not FrameInput: world/camera epochs and asset writer/read lifetimes
//! are not created by these bytes or by the observed producer invocation ID.
use core::mem::size_of;
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub enum CohortError {
    InvalidIdentity,
    BudgetExceeded,
    AllocationFailed,
    Invalidated,
    GroupPopulation,
    UnsortedShard,
    Incomplete,
}
#[derive(Clone, Debug, PartialEq, Eq)]
pub struct CohortHeader {
    pub vm: u64,
    pub source_invocation: u64,
    pub ppu_owner: u64,
    pub pass_occurrence: u64,
    /// Actual accepted execution-loop association; zero means unassociated.
    /// This is not a camera, asset or content publication epoch.
    pub execution_update: u64,
    pub camera: u32,
    pub group_counts: [[u32; 16]; 4],
}
#[derive(Clone, Debug, PartialEq, Eq)]
pub struct CohortItem {
    pub shard: u8,
    pub source_index: u16,
    pub mbi: [u8; 32],
    pub transform: Option<[u8; 112]>,
}
#[derive(Debug)]
pub struct OwnedVisibilityCohort {
    header: CohortHeader,
    items: Vec<CohortItem>,
}
impl OwnedVisibilityCohort {
    pub fn header(&self) -> &CohortHeader {
        &self.header
    }
    pub fn items(&self) -> &[CohortItem] {
        &self.items
    }
}
/// Main's authored semantic comparison key. This selects ordering only; it
/// does not interpret material/geometry resources or remove any material group.
fn key(item: &CohortItem) -> [u8; 16] {
    let mut result = [0; 16];
    if item.mbi[0] >> 4 == 15 {
        result.copy_from_slice(&item.mbi[..16]);
    } else {
        result[0] = item.mbi[0] & 0xf0;
        result[1..13].copy_from_slice(&item.mbi[4..16]);
        result[13..].copy_from_slice(&item.mbi[..3]);
    }
    result
}
pub struct CohortBuilder {
    header: CohortHeader,
    shards: [Vec<CohortItem>; 4],
    observed: [[u32; 16]; 4],
    valid: bool,
    total: usize,
}
impl CohortBuilder {
    pub fn new(
        header: CohortHeader,
        item_budget: usize,
        byte_budget: usize,
    ) -> Result<Self, CohortError> {
        if header.vm == 0
            || header.source_invocation == 0
            || header.ppu_owner == 0
            || header.pass_occurrence == 0
            || header.camera > 3
        {
            return Err(CohortError::InvalidIdentity);
        }
        let total = header
            .group_counts
            .iter()
            .flatten()
            .try_fold(0usize, |sum, &n| sum.checked_add(n as usize))
            .ok_or(CohortError::BudgetExceeded)?;
        // The builder plus finalized ordering temporarily own two allocations.
        total
            .checked_mul(size_of::<CohortItem>() * 2)
            .filter(|&n| n <= byte_budget && total <= item_budget)
            .ok_or(CohortError::BudgetExceeded)?;
        let mut shards = core::array::from_fn(|_| Vec::new());
        for (items, counts) in shards.iter_mut().zip(header.group_counts.iter()) {
            let count = counts.iter().map(|&n| n as usize).sum();
            items
                .try_reserve_exact(count)
                .map_err(|_| CohortError::AllocationFailed)?;
        }
        Ok(Self {
            header,
            shards,
            observed: [[0; 16]; 4],
            valid: true,
            total,
        })
    }
    pub fn push(
        &mut self,
        shard: u8,
        source_index: u16,
        mbi: [u8; 32],
        transform: Option<[u8; 112]>,
    ) -> Result<(), CohortError> {
        if !self.valid {
            return Err(CohortError::Invalidated);
        }
        let result = (|| {
            let index = usize::from(shard);
            if index >= 4 {
                return Err(CohortError::GroupPopulation);
            }
            let group = usize::from(mbi[0] >> 4);
            if self.observed[index][group] >= self.header.group_counts[index][group] {
                return Err(CohortError::GroupPopulation);
            }
            let item = CohortItem {
                shard,
                source_index,
                mbi,
                transform,
            };
            if self.shards[index]
                .last()
                .is_some_and(|prior| key(prior) > key(&item))
            {
                return Err(CohortError::UnsortedShard);
            }
            self.shards[index].push(item);
            self.observed[index][group] += 1;
            Ok(())
        })();
        if result.is_err() {
            self.valid = false;
        }
        result
    }
    pub fn finish(self) -> Result<OwnedVisibilityCohort, CohortError> {
        if !self.valid {
            return Err(CohortError::Invalidated);
        }
        if self.observed != self.header.group_counts {
            return Err(CohortError::Incomplete);
        }
        let mut items = Vec::new();
        items
            .try_reserve_exact(self.total)
            .map_err(|_| CohortError::AllocationFailed)?;
        let mut streams = self.shards.map(Vec::into_iter);
        let mut heads = core::array::from_fn::<_, 4, _>(|i| streams[i].next());
        let mut priority = [0usize, 1, 2, 3];
        let reorder = |order: &mut [usize; 4], heads: &[Option<CohortItem>; 4]| {
            order.sort_by_key(|&i| {
                heads[i]
                    .as_ref()
                    .map(|x| (false, key(x)))
                    .unwrap_or((true, [0; 16]))
            });
        };
        reorder(&mut priority, &heads);
        while items.len() < self.total {
            let first = priority[0];
            let item = heads[first].take().ok_or(CohortError::Incomplete)?;
            items.push(item);
            heads[first] = streams[first].next();
            reorder(&mut priority, &heads);
        }
        Ok(OwnedVisibilityCohort {
            header: self.header,
            items,
        })
    }
}
