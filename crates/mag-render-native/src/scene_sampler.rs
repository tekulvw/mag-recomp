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
fn key(s: &State) -> Vec<u8> {
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
    entries: BTreeMap<Vec<u8>, (Handle, u64)>,
    epoch: u64,
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
        let states: BTreeMap<_, _> = commands
            .passes()
            .iter()
            .flat_map(|p| &p.draws)
            .flat_map(|d| &d.textures)
            .filter_map(|t| match t.sampler {
                scene::Sampler::Explicit(s) => Some((key(&s), s)),
                _ => None,
            })
            .collect();
        if states.len() > 1024 {
            return Err(-5);
        }
        for (key, state) in &states {
            let status = unsafe { mag_native_sampler_state_validate(state) };
            if status != 0 {
                return Err(status);
            }
            if let Some((_, age)) = self.entries.get_mut(key) {
                *age = self.epoch;
                continue;
            }
            if self.entries.len() >= 1024 {
                let old = self
                    .entries
                    .iter()
                    .filter(|(k, _)| !states.contains_key(*k))
                    .min_by_key(|(_, (_, age))| *age)
                    .map(|(k, (h, _))| (k.clone(), *h))
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
            self.entries.insert(key.clone(), (handle, self.epoch));
        }
        let table: Vec<_> = states.keys().map(|key| self.entries[key].0).collect();
        let indices: BTreeMap<_, _> = states
            .keys()
            .cloned()
            .enumerate()
            .map(|(i, k)| (k, 0x80000000u32 | i as u32))
            .collect();
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
                        sampler: match t.sampler {
                            scene::Sampler::NearestClamp => 0,
                            scene::Sampler::LinearClamp => 1,
                            scene::Sampler::CompareLessEqual => 2,
                            scene::Sampler::Explicit(s) => indices[&key(&s)],
                        },
                        reserved: 0,
                    })
                    .collect()
            })
            .collect();
        Ok((textures, table))
    }
}
