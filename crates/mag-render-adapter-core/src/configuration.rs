//! Source-observation protocol for the bounded Shadow configuration lifetime.
//!
//! Only the source-pinned C++ observer emits these events in production. Event
//! replay tests validate the protocol; arbitrary event data is not evidence that
//! a game ran. A configuration publication is never a frame/camera readiness
//! certificate. Initialization return means the real original caller published
//! state 2 after its initializer call and full drain; teardown revokes first.
use std::{collections::BTreeMap, sync::Arc};

pub const CONFIG_ABI_VERSION: u32 = 1;
#[derive(Clone, Copy, Debug)]
#[repr(C)]
pub struct ConfigEvent {
    pub abi_version: u32,
    pub struct_bytes: u32,
    pub kind: u32,
    pub reserved0: u32,
    pub vm: u64,
    pub context: u64,
    pub owner: u64,
    pub invocation: u64,
    pub manager: u32,
    pub state_address: u32,
    pub state_value: u32,
    pub reserved1: u32,
    pub parameters: [u32; 3],
    pub reserved2: u32,
}
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
#[repr(u32)]
pub enum ConfigStatus {
    Accepted = 0,
    Invalid = 1,
    Stale = 2,
    NotReady = 3,
    Capacity = 4,
}
#[derive(Clone, Debug, PartialEq, Eq)]
pub struct InitializedShadowConfiguration {
    pub vm_observation: u64,
    pub initialization_invocation: u64,
    pub manager: u32,
    pub state_address: u32,
    pub parameters: [u32; 3],
}
#[derive(Clone)]
struct Pending {
    context: u64,
    owner: u64,
    invocation: u64,
    state: u32,
}
enum Entry {
    Pending(Pending),
    Published(Arc<InitializedShadowConfiguration>),
}
pub struct ConfigurationObserver {
    vm: u64,
    last_vm: u64,
    last_initialization: u64,
    maximum: usize,
    entries: BTreeMap<u32, Entry>,
}
impl ConfigurationObserver {
    pub fn new(maximum: usize) -> Self {
        Self {
            vm: 0,
            last_vm: 0,
            last_initialization: 0,
            maximum,
            entries: BTreeMap::new(),
        }
    }
    pub fn configuration(
        &self,
        vm: u64,
        manager: u32,
    ) -> Option<Arc<InitializedShadowConfiguration>> {
        if vm == 0 || vm != self.vm {
            return None;
        }
        match self.entries.get(&manager) {
            Some(Entry::Published(value)) => Some(value.clone()),
            _ => None,
        }
    }
    pub fn observe(&mut self, event: ConfigEvent) -> ConfigStatus {
        let well_formed = event.abi_version == CONFIG_ABI_VERSION
            && event.struct_bytes as usize == size_of::<ConfigEvent>()
            && event.reserved0 == 0
            && event.reserved1 == 0
            && event.reserved2 == 0
            && event.vm != 0;
        if !well_formed {
            return ConfigStatus::Invalid;
        }
        if event.kind == 0 {
            if event.vm <= self.last_vm {
                return ConfigStatus::Stale;
            }
            self.last_vm = event.vm;
            self.last_initialization = 0;
            self.entries.clear();
            self.vm = event.vm;
            return ConfigStatus::Accepted;
        }
        if self.vm != event.vm {
            return ConfigStatus::Stale;
        }
        if event.kind == 1 {
            self.vm = 0;
            self.entries.clear();
            return ConfigStatus::Accepted;
        }
        if event.manager < 0x10000
            || event.state_address < 0x10000
            || event.context == 0
            || event.invocation == 0
        {
            return ConfigStatus::Invalid;
        }
        if event.kind == 2 {
            if event.invocation <= self.last_initialization {
                return ConfigStatus::Stale;
            }
            if self.entries.len() >= self.maximum && !self.entries.contains_key(&event.manager) {
                return ConfigStatus::Capacity;
            }
            self.last_initialization = event.invocation;
            let pending = Pending {
                context: event.context,
                owner: event.owner,
                invocation: event.invocation,
                state: event.state_address,
            };
            self.entries.insert(event.manager, Entry::Pending(pending));
            return ConfigStatus::Accepted;
        }
        if matches!(event.kind, 4 | 5) {
            self.entries.remove(&event.manager);
            return ConfigStatus::Accepted;
        }
        if event.kind != 3 {
            return ConfigStatus::Invalid;
        }
        let Some(Entry::Pending(pending)) = self.entries.remove(&event.manager) else {
            return ConfigStatus::NotReady;
        };
        if (
            pending.context,
            pending.owner,
            pending.invocation,
            pending.state,
        ) != (
            event.context,
            event.owner,
            event.invocation,
            event.state_address,
        ) {
            return ConfigStatus::Stale;
        }
        if event.state_value != 2 {
            return ConfigStatus::NotReady;
        }
        let publication = InitializedShadowConfiguration {
            vm_observation: event.vm,
            initialization_invocation: event.invocation,
            manager: event.manager,
            state_address: event.state_address,
            parameters: event.parameters,
        };
        self.entries
            .insert(event.manager, Entry::Published(Arc::new(publication)));
        ConfigStatus::Accepted
    }
}
