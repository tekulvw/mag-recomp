//! One deformation per item, shared by Main and the depth pre-pass.
//!
//! Group 1's depth-equal test needs the depth pre-pass and Main to see bit-identical positions
//! for every deformed item. Each item's prepared banks are computed once, by the single
//! behaviour-crate function for its group, and both passes receive the same shared result
//! (including the same refusal). Group 0 and group 3 join here when their wind coefficients
//! are published; until then the behaviour crate refuses them.

use mag_render_behavior::noninstanced::{DeformError, DeformationInputs, DeformedBanks, deform};
use mag_render_behavior::skinning::{SkinnedBanks, SkinningError, SkinningInputs, skin};
use std::collections::HashMap;
use std::sync::Arc;

/// Identifies one item partition within one source window.
#[derive(Clone, Copy, Debug, PartialEq, Eq, Hash)]
pub struct DeformationKey {
    pub cohort: u64,
    pub item: u32,
    pub partition: u32,
}

/// A prepared item: the deformed banks of its group.
#[derive(Clone, Debug, PartialEq)]
pub enum Prepared {
    Ripple(DeformedBanks),
    Skinned(SkinnedBanks),
}

#[derive(Clone, Debug, PartialEq, Eq)]
pub enum PreparationError {
    Noninstanced(DeformError),
    Skinning(SkinningError),
}

pub type Shared = Arc<Result<Prepared, PreparationError>>;

/// Per-window store of prepared items. Both consumers call [`DeformationStore::prepare`] with the
/// same key and get the same shared result; the deformation runs at most once per key.
#[derive(Debug, Default)]
pub struct DeformationStore {
    entries: HashMap<DeformationKey, Shared>,
    runs: u64,
}

impl DeformationStore {
    pub fn new() -> Self {
        Self::default()
    }

    /// Returns the shared result for `key`, running `deformation` only the first time.
    pub fn prepare(
        &mut self,
        key: DeformationKey,
        deformation: impl FnOnce() -> Result<Prepared, PreparationError>,
    ) -> Shared {
        if let Some(shared) = self.entries.get(&key) {
            return Arc::clone(shared);
        }
        self.runs += 1;
        let shared = Arc::new(deformation());
        self.entries.insert(key, Arc::clone(&shared));
        shared
    }

    /// Number of deformations actually run.
    pub fn runs(&self) -> u64 {
        self.runs
    }

    /// Drops every entry at the end of a window.
    pub fn clear(&mut self) {
        self.entries.clear();
    }
}

/// The shared group-3/4 deformation.
pub fn noninstanced(
    inputs: &DeformationInputs<'_>,
    max_vertices: usize,
) -> Result<Prepared, PreparationError> {
    deform(inputs, max_vertices)
        .map(Prepared::Ripple)
        .map_err(PreparationError::Noninstanced)
}

/// The shared group-11 skinning.
pub fn skinned(
    inputs: &SkinningInputs<'_>,
    max_vertices: usize,
) -> Result<Prepared, PreparationError> {
    skin(inputs, max_vertices)
        .map(Prepared::Skinned)
        .map_err(PreparationError::Skinning)
}

#[cfg(test)]
mod tests {
    use super::*;
    use mag_render_behavior::noninstanced::DEFAULT_MAX_VERTICES;

    fn bits(values: [f32; 4]) -> [u32; 4] {
        values.map(f32::to_bits)
    }

    struct Ripple {
        positions: Vec<[u32; 4]>,
        normals: Vec<[u32; 4]>,
        weights: Vec<[u32; 4]>,
        tangents: Vec<[u32; 4]>,
    }

    fn ripple_item() -> Ripple {
        Ripple {
            positions: (0..64)
                .map(|i| bits([i as f32 * 0.37, 1.0 - i as f32 * 0.11, 0.25, 1.0]))
                .collect(),
            normals: vec![bits([0.0, 0.0, 1.0, 0.0]); 64],
            weights: vec![bits([1.0, 0.5, 0.125, 0.0]); 64],
            tangents: vec![bits([1.0, 0.0, 0.0, 1.0]); 64],
        }
    }

    fn inputs(item: &Ripple) -> DeformationInputs<'_> {
        DeformationInputs {
            group: 4,
            wind: [0, 0, 0, 0, 12345.678f32.to_bits()],
            material: [
                bits([1.0, 0.5, 0.25, 1.0]),
                bits([0.5, 0.75, 2.0, 0.0]),
                bits([0.25, 1.0, 4.0, 0.0]),
            ],
            origin: [3.3f32.to_bits(), 0.7f32.to_bits(), 1.9f32.to_bits()],
            positions: &item.positions,
            normals: &item.normals,
            weights: &item.weights,
            tangents: Some(&item.tangents),
        }
    }

    /// The depth pre-pass and Main consume bit-identical positions for a deformed item.
    #[test]
    fn depth_and_main_see_identical_positions() {
        let item = ripple_item();
        let key = DeformationKey {
            cohort: 1,
            item: 7,
            partition: 0,
        };
        let mut store = DeformationStore::new();
        let depth = store.prepare(key, || noninstanced(&inputs(&item), DEFAULT_MAX_VERTICES));
        let main = store.prepare(key, || noninstanced(&inputs(&item), DEFAULT_MAX_VERTICES));
        assert!(Arc::ptr_eq(&depth, &main));
        assert_eq!(store.runs(), 1);
        // A pass that cannot use the store and calls the same function gets the same bits.
        let independent = noninstanced(&inputs(&item), DEFAULT_MAX_VERTICES).unwrap();
        let (Ok(Prepared::Ripple(shared)), Prepared::Ripple(own)) = (&*main, &independent) else {
            panic!("ripple expected")
        };
        assert_eq!(shared.positions, own.positions);
        // And the positions really moved.
        assert_ne!(shared.positions, item.positions);
    }

    #[test]
    fn refusals_are_shared_too() {
        let item = ripple_item();
        let mut store = DeformationStore::new();
        let key = DeformationKey {
            cohort: 1,
            item: 8,
            partition: 0,
        };
        let mut bad = inputs(&item);
        bad.group = 3;
        let first = store.prepare(key, || noninstanced(&bad, DEFAULT_MAX_VERTICES));
        assert_eq!(
            *first,
            Err(PreparationError::Noninstanced(
                DeformError::HeldWindConstants
            ))
        );
        let second = store.prepare(key, || unreachable!("prepared once"));
        assert!(Arc::ptr_eq(&first, &second));
        store.clear();
        let other = store.prepare(key, || noninstanced(&inputs(&item), DEFAULT_MAX_VERTICES));
        assert!(other.is_ok());
    }

    #[test]
    fn skinning_goes_through_the_same_store() {
        let matrix: Vec<u8> = [
            [1.0f32, 0.0, 0.0, 1.0],
            [0.0, 1.0, 0.0, 2.0],
            [0.0, 0.0, 1.0, 3.0],
        ]
        .iter()
        .flatten()
        .flat_map(|v| v.to_bits().to_be_bytes())
        .collect();
        let positions = [[0.5f32, 0.25, 0.125, 1.0]];
        let zero = [[0.0f32; 4]];
        let skin_inputs = SkinningInputs {
            segments: [&matrix, &[]],
            influences: &[255, 0, 0, 0, 0, 0, 0, 0],
            positions: &positions,
            normals: &zero,
            tangents: &zero,
            object: [0, 0, 0, 1f32.to_bits(), 0, 0, 0, 1f32.to_bits()],
        };
        let mut store = DeformationStore::new();
        let key = DeformationKey {
            cohort: 2,
            item: 1,
            partition: 3,
        };
        let depth = store.prepare(key, || skinned(&skin_inputs, DEFAULT_MAX_VERTICES));
        let main = store.prepare(key, || skinned(&skin_inputs, DEFAULT_MAX_VERTICES));
        assert!(Arc::ptr_eq(&depth, &main));
        let Ok(Prepared::Skinned(banks)) = &*main else {
            panic!("skinned expected")
        };
        assert_eq!(banks.positions[0], [1.5, 2.25, 3.125, 1.0]);
    }
}
