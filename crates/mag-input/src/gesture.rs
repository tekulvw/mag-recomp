//! Per-consumer recognition over immutable offers and normalized contributions.
use crate::delivery::Offer;
#[derive(Clone, Copy, Debug, PartialEq)]
#[repr(C)]
pub struct Config {
    pub press_delay_ns: u64,
    pub double_tap_gap_ns: u64,
    pub repeat_delay_ns: u64,
    pub repeat_interval_ns: u64,
    pub refocus_delay_ns: u64,
    pub down_threshold: f64,
    pub change_epsilon: f64,
}
impl Default for Config {
    fn default() -> Self {
        Self {
            press_delay_ns: 0,
            double_tap_gap_ns: 0,
            repeat_delay_ns: 0,
            repeat_interval_ns: 0,
            refocus_delay_ns: 0,
            down_threshold: 0.5,
            change_epsilon: 0.0,
        }
    }
}
#[derive(Clone, Copy, Debug, Default, Eq, PartialEq)]
#[repr(u32)]
pub enum Phase {
    #[default]
    Idle,
    ReleasedTap,
    Down,
    SecondDown,
    Pressed,
    SecondPressed,
    Blocked,
    Refocus,
}
pub struct Events;
impl Events {
    pub const DOWN: u32 = 1;
    pub const UP: u32 = 1 << 1;
    pub const CHANGE: u32 = 1 << 2;
    pub const PRESS: u32 = 1 << 3;
    pub const RELEASE: u32 = 1 << 4;
    pub const TAP: u32 = 1 << 5;
    pub const DOUBLE_TAP: u32 = 1 << 6;
    pub const SINGLE_TAP: u32 = 1 << 7;
    pub const REPEAT: u32 = 1 << 8;
    pub const TAP_DOWN: u32 = 1 << 9;
    pub const TAP_PRESS: u32 = 1 << 10;
    pub const TAP_RELEASE: u32 = 1 << 11;
    pub const WHILE_DOWN: u32 = 1 << 12;
    pub const WHILE_PRESSED: u32 = 1 << 13;
}
#[derive(Clone, Copy, Debug, Default, PartialEq)]
pub struct Output {
    pub value: f64,
    pub elapsed_ns: u64,
    pub phase: Phase,
    pub events: u32,
}
#[derive(Clone, Copy, Debug, Eq, PartialEq)]
pub enum Error {
    InvalidValue,
    InvalidTime,
    ChangedFrame,
}
#[derive(Clone, Copy, Debug, PartialEq)]
struct Input {
    sequence: u64,
    now: u64,
    offer: Option<Offer>,
    value: f64,
    blocked: bool,
    config: Config,
}
#[derive(Default)]
pub struct Recognizer {
    cached: Option<(Input, Output)>,
    down: bool,
    pressed: bool,
    second: bool,
    blocked: bool,
    down_time: u64,
    elapsed: u64,
    last_tap: Option<u64>,
    last_repeat: Option<u64>,
    last_value: f64,
}
impl Recognizer {
    pub fn update(
        &mut self,
        sequence: u64,
        now: u64,
        offer: Option<Offer>,
        value: f64,
        blocked: bool,
        config: Config,
    ) -> Result<Output, Error> {
        if !value.is_finite()
            || !(0.0..=1.0).contains(&value)
            || !config.down_threshold.is_finite()
            || !(0.0..=1.0).contains(&config.down_threshold)
            || !config.change_epsilon.is_finite()
            || config.change_epsilon < 0.0
        {
            return Err(Error::InvalidValue);
        }
        let input = Input {
            sequence,
            now,
            offer,
            value,
            blocked,
            config,
        };
        if let Some((previous, output)) = self.cached {
            if sequence == previous.sequence {
                return if input == previous {
                    Ok(output)
                } else {
                    Err(Error::ChangedFrame)
                };
            }
            if sequence < previous.sequence || now < previous.now {
                return Err(Error::InvalidTime);
            }
        }
        if sequence == 0 {
            return Err(Error::InvalidTime);
        }
        let output = self.advance(input);
        self.cached = Some((input, output));
        Ok(output)
    }
    fn advance(&mut self, input: Input) -> Output {
        let Input {
            now,
            offer,
            value,
            blocked,
            config,
            ..
        } = input;
        if blocked {
            self.down = false;
            self.pressed = false;
            self.blocked = true;
            self.last_tap = None;
            self.last_value = 0.0;
            return Output {
                phase: Phase::Blocked,
                ..Output::default()
            };
        }
        if self.blocked {
            if value > 0.0 {
                return Output {
                    phase: Phase::Refocus,
                    ..Output::default()
                };
            }
            self.blocked = false;
            self.elapsed = 0;
            self.last_value = 0.0;
            return Output::default();
        }
        let down = value > 0.0 && value >= config.down_threshold;
        let mut events = if (self.last_value - value).abs() > config.change_epsilon {
            Events::CHANGE
        } else {
            0
        };
        self.last_value = value;
        if down && !self.down {
            self.down_time = now;
            self.elapsed = 0;
            self.pressed = false;
            self.last_repeat = None;
            self.second = self
                .last_tap
                .is_some_and(|tap| now.saturating_sub(tap) <= config.double_tap_gap_ns);
            if self.second {
                self.last_tap = None;
                events |= Events::TAP_DOWN
            }
            events |= Events::DOWN;
        }
        if down {
            self.elapsed = offer.filter(|offer| offer.down).map_or_else(
                || now.saturating_sub(self.down_time).max(self.elapsed),
                |offer| offer.physical_duration_ns.max(self.elapsed),
            );
            events |= Events::WHILE_DOWN;
            if self.elapsed >= config.press_delay_ns {
                if !self.pressed {
                    events |= Events::PRESS;
                    if self.second {
                        events |= Events::TAP_PRESS
                    }
                }
                self.pressed = true;
                events |= Events::WHILE_PRESSED;
            }
            if config.repeat_interval_ns > 0 && self.elapsed >= config.repeat_delay_ns {
                let repeat = (self.elapsed - config.repeat_delay_ns) / config.repeat_interval_ns;
                if self.last_repeat.is_none_or(|last| repeat > last) {
                    events |= Events::REPEAT;
                    self.last_repeat = Some(repeat)
                }
            }
        } else if self.down {
            self.elapsed = offer.map_or(
                self.elapsed.max(now.saturating_sub(self.down_time)),
                |offer| offer.physical_duration_ns.max(self.elapsed),
            );
            events |= Events::UP;
            if self.pressed {
                events |= Events::RELEASE;
                if self.second {
                    events |= Events::TAP_RELEASE
                }
            } else {
                events |= Events::TAP;
                if self.second {
                    events |= Events::DOUBLE_TAP;
                    self.last_tap = None
                } else if config.double_tap_gap_ns == 0 {
                    events |= Events::SINGLE_TAP
                } else {
                    self.last_tap = Some(now)
                }
            }
        }
        if !down
            && self
                .last_tap
                .is_some_and(|tap| now.saturating_sub(tap) > config.double_tap_gap_ns)
        {
            events |= Events::SINGLE_TAP;
            self.last_tap = None
        }
        let phase = if down {
            match (self.pressed, self.second) {
                (false, false) => Phase::Down,
                (false, true) => Phase::SecondDown,
                (true, false) => Phase::Pressed,
                (true, true) => Phase::SecondPressed,
            }
        } else if events & Events::TAP != 0 {
            Phase::ReleasedTap
        } else {
            Phase::Idle
        };
        self.down = down;
        Output {
            value,
            elapsed_ns: self.elapsed,
            phase,
            events,
        }
    }
}
