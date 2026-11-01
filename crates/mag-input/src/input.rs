use crate::delivery::Offer;

#[derive(Clone, Copy, Debug, Eq, Ord, PartialEq, PartialOrd)]
pub enum Source {
    Keyboard,
    Mouse,
    Controller,
}

impl Source {
    pub const fn bit(self) -> u8 {
        1 << (self as u8)
    }
}

#[derive(Clone, Copy, Debug, Eq, Ord, PartialEq, PartialOrd)]
pub struct Control {
    pub source: Source,
    pub code: u32,
}

#[derive(Clone, Copy, Debug, Eq, Ord, PartialEq, PartialOrd)]
pub(crate) struct DeviceControl {
    pub device: u64,
    pub control: Control,
}

#[derive(Clone, Copy, Debug, Eq, PartialEq)]
pub struct Event {
    pub device: u64,
    pub control: Control,
    pub down: bool,
    pub time_ns: u64,
    pub generation: u64,
}

impl Event {
    pub(crate) fn device_control(self) -> DeviceControl {
        DeviceControl {
            device: self.device,
            control: self.control,
        }
    }
}

#[derive(Clone, Copy, Debug, Default, Eq, PartialEq)]
pub struct Capture {
    pub focused: bool,
    pub supported_context: bool,
    pub mouse_captured: bool,
    pub controller_connected: bool,
}

impl Capture {
    pub(crate) fn active(self) -> bool {
        self.focused && self.supported_context
    }
}

#[derive(Clone, Copy, Debug, Eq, PartialEq)]
#[repr(u32)]
pub enum Context {
    OnFoot = 1,
    Vehicle = 2,
    Turret = 4,
    Tactical = 8,
    Menu = 16,
    Settings = 32,
    Rebinding = 64,
}

impl Context {
    pub const fn bit(self) -> u32 {
        self as u32
    }
}

#[derive(Clone, Copy, Debug, Eq, PartialEq)]
pub struct ContextKey {
    pub owner: u64,
    pub context: Context,
}

#[derive(Clone, Copy, Debug, Eq, PartialEq)]
pub struct FrameKey {
    pub session: u64,
    pub sequence: u64,
    pub player_slot: u32,
}

#[derive(Clone, Copy, Debug, Default, Eq, PartialEq)]
pub struct Capabilities {
    pub look: bool,
}

#[derive(Clone, Copy, Debug, Default, PartialEq)]
pub struct MouseMotion {
    pub dx: f64,
    pub dy: f64,
}

#[derive(Clone, Debug, PartialEq)]
pub struct Frame {
    pub key: FrameKey,
    pub generation: u64,
    pub context: ContextKey,
    pub actions: Vec<(u32, Offer)>,
    pub mouse: MouseMotion,
    pub(crate) look_allowed: bool,
}

impl Frame {
    pub fn action(&self, action: u32) -> Option<Offer> {
        self.actions
            .iter()
            .find_map(|(id, offer)| (*id == action).then_some(*offer))
    }

    pub fn look(&self, capabilities: Capabilities) -> Option<MouseMotion> {
        (self.look_allowed && capabilities.look).then_some(self.mouse)
    }
}
