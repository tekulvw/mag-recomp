//! Accepted gameplay input, rather than provisional routing, controls aim assist.
#[derive(Clone, Copy, Debug, Default, Eq, PartialEq)]
#[repr(u32)]
pub enum Mode {
    #[default]
    Auto,
    Controller,
    Kbm,
}
#[derive(Clone, Copy, Debug)]
pub struct Policy {
    pub mode: Mode,
    pub mouse_owner: bool,
    pub kbm_used: bool,
    pub stick_rearmed: bool,
    pub controller_connected: bool,
}
impl Default for Policy {
    fn default() -> Self {
        Self {
            mode: Mode::Auto,
            mouse_owner: false,
            kbm_used: false,
            stick_rearmed: true,
            controller_connected: false,
        }
    }
}
impl Policy {
    pub fn set_mode(&mut self, mode: Mode) {
        self.mode = mode;
        match mode {
            Mode::Kbm => {
                self.kbm_used = true;
                self.mouse_owner = true
            }
            Mode::Controller => self.mouse_owner = false,
            Mode::Auto => {}
        }
    }
    pub fn assist_allowed(self) -> bool {
        self.mode == Mode::Controller || (self.mode == Mode::Auto && !self.kbm_used)
    }
    pub fn accept_sources(&mut self, sources: u8) {
        if self.mode != Mode::Controller && sources & 3 != 0 {
            self.kbm_used = true
        }
    }
    pub fn accept_mouse(&mut self, neutral: bool) {
        if self.mode != Mode::Controller {
            self.kbm_used = true;
            self.mouse_owner = true;
            self.stick_rearmed = neutral
        }
    }
    pub fn update_capture(&mut self, active: bool, connected: bool, neutral: bool, fresh: bool) {
        self.controller_connected = connected;
        match self.mode {
            Mode::Controller => {
                self.mouse_owner = false;
                self.stick_rearmed = neutral
            }
            Mode::Kbm => {
                self.mouse_owner = true;
                self.stick_rearmed = neutral
            }
            Mode::Auto => {
                if neutral {
                    self.stick_rearmed = true
                }
                if active && connected && fresh && self.stick_rearmed {
                    self.mouse_owner = false;
                    self.stick_rearmed = false
                } else if !connected {
                    self.mouse_owner = true
                }
            }
        }
    }
}
