//! Exact immutable sampler cache and shallow generation-qualified submission
//! table. Native pending work owns independent sampler state through terminal.
use super::*;
pub type State = mag_render_abi::packet::Sampler;
#[repr(C)]
#[derive(Clone, Copy, Default)]
pub struct Handle {
    pub generation: u64,
    pub id: u64,
}
#[repr(C)]
pub struct Desc {
    pub struct_size: u32,
    pub abi_version: u32,
    pub state: State,
}
#[repr(C)]
pub(super) struct SceneV2 {
    pub struct_size: u32,
    pub abi_version: u32,
    pub scene: *const Scene,
    pub samplers: *const Handle,
    pub sampler_count: u32,
    pub reserved: u32,
}
unsafe extern "C" {
    fn mag_native_sampler_create(device: *mut c_void, desc: *const Desc, out: *mut Handle) -> i32;
    fn mag_native_sampler_release(device: *mut c_void, handle: Handle) -> i32;
    fn mag_native_sampler_state_validate(state: *const State) -> i32;
    fn mag_native_sampler_layout(out: *mut u32, words: u32) -> i32;
    pub(super) fn mag_native_submit_scene_v2(
        device: *mut c_void,
        scene: *const SceneV2,
        target: *const crate::host_image::NativeImage,
        frame: *const crate::host_frame::HostFrame,
        out: *mut Ticket,
    ) -> i32;
}
/// The cache key: the state's fields in a fixed order, little-endian, floats
/// by bit pattern (so -0.0, 0.0 and distinct NaN payloads stay distinct). A
/// fixed array, so building, comparing and storing one allocates nothing; its
/// byte order is the order the table has always used.
const KEY_BYTES: usize = 8 + 8 * 4 + 8 * 4;
type Key = [u8; KEY_BYTES];
fn key(s: &State) -> Key {
    let mut k = [0u8; KEY_BYTES];
    k[..8].copy_from_slice(&s.known_fields.to_le_bytes());
    let words = [
        s.min_filter,
        s.mag_filter,
        s.mip_filter,
        s.wrap_s,
        s.wrap_t,
        s.wrap_r,
        s.comparison_enabled,
        s.comparison,
        s.lod_min.to_bits(),
        s.lod_max.to_bits(),
        s.lod_bias.to_bits(),
        s.max_anisotropy.to_bits(),
        s.border[0].to_bits(),
        s.border[1].to_bits(),
        s.border[2].to_bits(),
        s.border[3].to_bits(),
    ];
    for (i, word) in words.into_iter().enumerate() {
        k[8 + 4 * i..12 + 4 * i].copy_from_slice(&word.to_le_bytes());
    }
    k
}
/// The distinct explicit states of one submission in key order, the last
/// occurrence of each key winning (as collecting into an ordered map did).
/// Reused across submissions: cleared, never reallocated once warm.
fn distinct_states<'a>(scratch: &mut Vec<(Key, State)>, states: impl Iterator<Item = &'a State>) {
    scratch.clear();
    scratch.extend(states.map(|s| (key(s), *s)));
    // Stable, so equal keys keep their submission order; keep the last.
    scratch.sort_by_key(|e| e.0);
    let mut write = 0;
    for read in 0..scratch.len() {
        if write > 0 && scratch[write - 1].0 == scratch[read].0 {
            scratch[write - 1] = scratch[read];
        } else {
            scratch[write] = scratch[read];
            write += 1;
        }
    }
    scratch.truncate(write);
}
fn table_index(distinct: &[(Key, State)], state: &State) -> u32 {
    let k = key(state);
    let i = distinct
        .binary_search_by(|e| e.0.cmp(&k))
        .expect("every explicit state was collected");
    0x80000000u32 | i as u32
}
pub(super) fn checked_layout() -> Result<(), i32> {
    let expected = [
        size_of::<Handle>() as u32,
        align_of::<Handle>() as u32,
        offset_of!(Handle, id) as u32,
        size_of::<State>() as u32,
        align_of::<State>() as u32,
        offset_of!(State, lod_min) as u32,
        offset_of!(State, border) as u32,
        size_of::<Desc>() as u32,
        align_of::<Desc>() as u32,
        offset_of!(Desc, state) as u32,
        size_of::<SceneV2>() as u32,
        align_of::<SceneV2>() as u32,
        offset_of!(SceneV2, scene) as u32,
        offset_of!(SceneV2, samplers) as u32,
        offset_of!(SceneV2, sampler_count) as u32,
    ];
    let mut actual = [0; 15];
    let status = unsafe { mag_native_sampler_layout(actual.as_mut_ptr(), actual.len() as u32) };
    if status != 0 {
        return Err(status);
    }
    if actual != expected {
        return Err(-1);
    }
    Ok(())
}
#[derive(Default)]
pub(super) struct Cache {
    entries: BTreeMap<Key, (Handle, u64)>,
    epoch: u64,
    distinct: Vec<(Key, State)>,
}
impl Cache {
    pub(super) fn release_all(&mut self, api: SdkApi) {
        for (h, _) in self.entries.values() {
            unsafe { mag_native_sampler_release(api.device, *h) };
        }
        self.entries.clear();
    }
    pub(super) fn resolve(
        &mut self,
        api: SdkApi,
        commands: &scene::SceneCommands,
    ) -> Result<(Vec<Vec<TextureBinding>>, Vec<Handle>), i32> {
        checked_layout()?;
        self.epoch = self.epoch.checked_add(1).ok_or(-5)?;
        distinct_states(
            &mut self.distinct,
            commands
                .passes()
                .iter()
                .flat_map(|p| &p.draws)
                .flat_map(|d| &d.textures)
                .filter_map(|t| match &t.sampler {
                    scene::Sampler::Explicit(s) => Some(s),
                    _ => None,
                }),
        );
        if self.distinct.len() > 1024 {
            return Err(-5);
        }
        for (key, state) in &self.distinct {
            let status = unsafe { mag_native_sampler_state_validate(state) };
            if status != 0 {
                return Err(status);
            }
            if let Some((_, age)) = self.entries.get_mut(key) {
                *age = self.epoch;
                continue;
            }
            if self.entries.len() >= 1024 {
                let distinct = &self.distinct;
                let old = self
                    .entries
                    .iter()
                    .filter(|(k, _)| distinct.binary_search_by(|e| e.0.cmp(k)).is_err())
                    .min_by_key(|(_, (_, age))| *age)
                    .map(|(k, (h, _))| (*k, *h))
                    .ok_or(-5)?;
                let status = unsafe { mag_native_sampler_release(api.device, old.1) };
                if status != 0 {
                    return Err(status);
                }
                self.entries.remove(&old.0);
            }
            let desc = Desc {
                struct_size: size_of::<Desc>() as u32,
                abi_version: 1,
                state: *state,
            };
            let mut handle = Handle::default();
            let status = unsafe { mag_native_sampler_create(api.device, &desc, &mut handle) };
            if status != 0 {
                return Err(status);
            }
            self.entries.insert(*key, (handle, self.epoch));
        }
        let table: Vec<_> = self
            .distinct
            .iter()
            .map(|(key, _)| self.entries[key].0)
            .collect();
        let distinct = &self.distinct;
        let textures = commands
            .passes()
            .iter()
            .flat_map(|p| &p.draws)
            .map(|d| {
                d.textures
                    .iter()
                    .map(|t| TextureBinding {
                        stage: t.stage as u32,
                        texture_slot: t.texture_slot,
                        sampler_slot: t.sampler_slot,
                        image: t.image.image().index() as u32,
                        sampler: match &t.sampler {
                            scene::Sampler::NearestClamp => 0,
                            scene::Sampler::LinearClamp => 1,
                            scene::Sampler::CompareLessEqual => 2,
                            scene::Sampler::Explicit(s) => table_index(distinct, s),
                        },
                        reserved: 0,
                    })
                    .collect()
            })
            .collect();
        Ok((textures, table))
    }
}
#[cfg(test)]
mod tests {
    use super::*;
    /// The previous resolution: heap keys, the distinct states collected into
    /// an ordered map, indices from a second map.
    fn reference_key(s: &State) -> Vec<u8> {
        let mut k = Vec::new();
        k.extend(s.known_fields.to_le_bytes());
        for x in [
            s.min_filter,
            s.mag_filter,
            s.mip_filter,
            s.wrap_s,
            s.wrap_t,
            s.wrap_r,
            s.comparison_enabled,
            s.comparison,
        ] {
            k.extend(x.to_le_bytes())
        }
        for f in [s.lod_min, s.lod_max, s.lod_bias, s.max_anisotropy]
            .into_iter()
            .chain(s.border)
        {
            k.extend(f.to_bits().to_le_bytes())
        }
        k
    }
    fn reference(states: &[State]) -> (Vec<State>, Vec<u32>) {
        let map: BTreeMap<_, _> = states.iter().map(|s| (reference_key(s), *s)).collect();
        let indices: BTreeMap<_, _> = map
            .keys()
            .cloned()
            .enumerate()
            .map(|(i, k)| (k, 0x80000000u32 | i as u32))
            .collect();
        (
            map.values().copied().collect(),
            states.iter().map(|s| indices[&reference_key(s)]).collect(),
        )
    }
    fn state(seed: u64) -> State {
        let mut x = seed.wrapping_mul(0x9E3779B97F4A7C15) | 1;
        let mut next = || {
            x ^= x << 13;
            x ^= x >> 7;
            x ^= x << 17;
            x
        };
        // Few values per field, so collisions and near-collisions are common.
        let f = |v: u64| -> f32 {
            match v % 8 {
                0 => 0.0,
                1 => -0.0,
                2 => f32::NAN,
                3 => f32::from_bits(0x7fc0_0001),
                4 => f32::INFINITY,
                5 => 1.0,
                6 => -1000.5,
                _ => f32::from_bits(1),
            }
        };
        State {
            known_fields: next() % 3,
            min_filter: (next() % 3) as u32,
            mag_filter: (next() % 2) as u32,
            mip_filter: (next() % 3) as u32,
            wrap_s: (next() % 4) as u32,
            wrap_t: (next() % 2) as u32,
            wrap_r: (next() % 2) as u32 * 0x0100_0000,
            comparison_enabled: (next() % 2) as u32,
            comparison: (next() % 2) as u32,
            lod_min: f(next()),
            lod_max: f(next()),
            lod_bias: f(next()),
            max_anisotropy: f(next()),
            border: [f(next()), f(next()), f(next()), f(next())],
        }
    }
    fn bits(s: &State) -> Vec<u8> {
        reference_key(s)
    }
    #[test]
    fn fixed_key_has_the_reference_bytes() {
        for seed in 0..2000 {
            let s = state(seed);
            assert_eq!(key(&s).as_slice(), reference_key(&s).as_slice());
        }
    }
    #[test]
    fn distinct_states_and_indices_match_the_reference() {
        let mut scratch = Vec::new();
        for round in 0..400u64 {
            let len = (round % 37) as usize;
            // Draw from a small pool so the same state repeats within a submission.
            let pool: Vec<State> = (0..1 + round % 9).map(|i| state(round * 31 + i)).collect();
            let states: Vec<State> = (0..len)
                .map(|i| pool[(i * 7 + round as usize) % pool.len()])
                .collect();
            distinct_states(&mut scratch, states.iter());
            let (ref_states, ref_indices) = reference(&states);
            assert_eq!(scratch.len(), ref_states.len());
            for ((_, got), want) in scratch.iter().zip(&ref_states) {
                assert_eq!(bits(got), bits(want));
            }
            let got: Vec<u32> = states.iter().map(|s| table_index(&scratch, s)).collect();
            assert_eq!(got, ref_indices);
        }
    }
    #[test]
    fn edge_states_stay_distinct_by_bit_pattern() {
        let base = state(7);
        let mut zero = base;
        zero.lod_bias = 0.0;
        let mut negative_zero = base;
        negative_zero.lod_bias = -0.0;
        let mut nan = base;
        nan.lod_bias = f32::NAN;
        let mut other_nan = base;
        other_nan.lod_bias = f32::from_bits(0x7fc0_0001);
        let states = [zero, negative_zero, nan, other_nan, nan, zero];
        let mut scratch = Vec::new();
        distinct_states(&mut scratch, states.iter());
        assert_eq!(scratch.len(), 4);
        let (ref_states, ref_indices) = reference(&states);
        assert_eq!(scratch.len(), ref_states.len());
        let got: Vec<u32> = states.iter().map(|s| table_index(&scratch, s)).collect();
        assert_eq!(got, ref_indices);
        // Scratch is reused: a smaller submission leaves no stale entries.
        distinct_states(&mut scratch, [zero].iter());
        assert_eq!(scratch.len(), 1);
        distinct_states(&mut scratch, std::iter::empty());
        assert!(scratch.is_empty());
    }
}
