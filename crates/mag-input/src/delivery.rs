//! Timestamped physical pulses offered until their native consumer observes them.

use std::collections::{BTreeMap, BTreeSet, VecDeque};
use std::fmt;

const MAX_ACTIONS: usize = 1024;
const MAX_PULSES: usize = 4096;
const MAX_PULSES_PER_ACTION: usize = 256;

#[derive(Clone, Copy, Debug, Eq, PartialEq)]
pub enum Error {
    InvalidAction,
    InvalidTransition,
    NonMonotonicTime,
    CapacityExceeded,
    StaleObservation,
}

impl fmt::Display for Error {
    fn fmt(&self, output: &mut fmt::Formatter<'_>) -> fmt::Result {
        output.write_str(match self {
            Self::InvalidAction => "action zero is reserved",
            Self::InvalidTransition => "physical action transition is not ordered",
            Self::NonMonotonicTime => "input time moved backwards",
            Self::CapacityExceeded => "bounded input capacity exceeded",
            Self::StaleObservation => "observation does not identify the current pulse",
        })
    }
}

impl std::error::Error for Error {}

#[derive(Clone, Copy, Debug, Eq, Ord, PartialEq, PartialOrd)]
pub struct Contributor {
    pub source: u8,
    pub device: u64,
    pub code: u32,
}

#[derive(Clone, Copy, Debug, Eq, PartialEq)]
pub struct Offer {
    pub pulse_id: u64,
    pub down: bool,
    pub physical_duration_ns: u64,
    pub source_mask: u8,
}

#[derive(Debug)]
struct Pulse {
    id: u64,
    down_time: u64,
    up_time: Option<u64>,
    presenting_up: bool,
    observed: bool,
    contributors: BTreeSet<Contributor>,
}

#[derive(Debug, Default)]
struct Action {
    pulses: VecDeque<Pulse>,
    last_event: Option<u64>,
    physically_down: bool,
}

#[derive(Debug, Default)]
pub struct Delivery {
    actions: BTreeMap<u32, Action>,
    next_id: u64,
    pulse_count: usize,
    now: Option<u64>,
    newest_event: u64,
}

impl Delivery {
    /// Queue a logical physical edge; aggregation precedes this operation.
    pub fn push(&mut self, action: u32, down: bool, time_ns: u64) -> Result<(), Error> {
        self.push_from(action, down, time_ns, BTreeSet::new())
    }

    pub fn push_from(
        &mut self,
        action: u32,
        down: bool,
        time_ns: u64,
        contributors: BTreeSet<Contributor>,
    ) -> Result<(), Error> {
        if action == 0 {
            return Err(Error::InvalidAction);
        }
        let current = self.actions.get(&action);
        if current.is_some_and(|state| state.last_event.is_some_and(|last| time_ns < last)) {
            return Err(Error::NonMonotonicTime);
        }
        if current.is_some_and(|state| state.physically_down) == down {
            return Err(Error::InvalidTransition);
        }
        if !self.actions.contains_key(&action) && self.actions.len() >= MAX_ACTIONS {
            return Err(Error::CapacityExceeded);
        }
        if down
            && (self.pulse_count >= MAX_PULSES
                || current.is_some_and(|state| state.pulses.len() >= MAX_PULSES_PER_ACTION))
        {
            return Err(Error::CapacityExceeded);
        }
        let id = if down {
            Some(self.next_id.checked_add(1).ok_or(Error::CapacityExceeded)?)
        } else {
            None
        };
        let state = self.actions.entry(action).or_default();
        if let Some(id) = id {
            self.next_id = id;
            state.pulses.push_back(Pulse {
                id,
                down_time: time_ns,
                up_time: None,
                presenting_up: false,
                observed: false,
                contributors,
            });
            self.pulse_count += 1;
        } else {
            let pulse = state.pulses.back_mut().ok_or(Error::InvalidTransition)?;
            pulse.up_time = Some(time_ns);
        }
        state.physically_down = down;
        state.last_event = Some(time_ns);
        self.newest_event = self.newest_event.max(time_ns);
        Ok(())
    }

    /// Advance presentation once for a new simulation opportunity.
    pub fn advance(&mut self, now_ns: u64) -> Result<(), Error> {
        if now_ns < self.newest_event || self.now.is_some_and(|last| now_ns < last) {
            return Err(Error::NonMonotonicTime);
        }
        if self.now == Some(now_ns) {
            return Ok(());
        }
        self.now = Some(now_ns);
        for state in self.actions.values_mut() {
            let Some(pulse) = state.pulses.front_mut() else {
                continue;
            };
            if !pulse.observed {
                continue;
            }
            if pulse.presenting_up {
                state.pulses.pop_front();
                self.pulse_count -= 1;
            } else if pulse.up_time.is_some() {
                pulse.presenting_up = true;
                pulse.observed = false;
            }
        }
        Ok(())
    }

    /// A read is repeatable and never acknowledges or drains input.
    pub fn offer(&self, action: u32) -> Option<Offer> {
        let pulse = self.actions.get(&action)?.pulses.front()?;
        let now = self.now?;
        Some(Offer {
            pulse_id: pulse.id,
            down: !pulse.presenting_up,
            physical_duration_ns: pulse.up_time.unwrap_or(now).saturating_sub(pulse.down_time),
            source_mask: pulse
                .contributors
                .iter()
                .fold(0, |sources, contributor| sources | contributor.source),
        })
    }

    pub fn observe(&mut self, action: u32, pulse_id: u64, down: bool) -> Result<(), Error> {
        let pulse = self
            .actions
            .get_mut(&action)
            .and_then(|state| state.pulses.front_mut())
            .filter(|pulse| pulse.id == pulse_id && pulse.presenting_up != down)
            .ok_or(Error::StaleObservation)?;
        pulse.observed = true;
        Ok(())
    }

    /// Cancel input without manufacturing an Up, Tap or gameplay event.
    pub fn cancel(&mut self) {
        self.actions.clear();
        self.pulse_count = 0;
        self.now = None;
        self.newest_event = 0;
        // Never reuse pulse identities, including across capture resets.
    }

    pub fn update_contributors(
        &mut self,
        action: u32,
        contributors: &BTreeSet<Contributor>,
    ) -> Result<(), Error> {
        let pulse = self
            .actions
            .get_mut(&action)
            .and_then(|state| state.pulses.back_mut())
            .filter(|pulse| pulse.up_time.is_none())
            .ok_or(Error::InvalidTransition)?;
        pulse.contributors.clone_from(contributors);
        Ok(())
    }

    /// Revoke only the matching devices, without emitting a gameplay release.
    pub fn cancel_sources(&mut self, sources: u8, device: Option<u64>) {
        for state in self.actions.values_mut() {
            state.pulses.retain_mut(|pulse| {
                if pulse.contributors.is_empty() {
                    return true;
                }
                pulse.contributors.retain(|contributor| {
                    contributor.source & sources == 0
                        || device.is_some_and(|id| id != contributor.device)
                });
                if !pulse.contributors.is_empty() {
                    return true;
                }
                self.pulse_count -= 1;
                false
            });
            state.physically_down = state
                .pulses
                .back()
                .is_some_and(|pulse| pulse.up_time.is_none());
        }
    }

    pub(crate) fn current_offer_retained(
        &self,
        action: u32,
        allowed: impl Fn(&Contributor) -> bool,
    ) -> bool {
        self.actions
            .get(&action)
            .and_then(|state| state.pulses.front())
            .is_some_and(|pulse| pulse.contributors.iter().any(allowed))
    }

    pub(crate) fn retain_contributors(&mut self, allowed: impl Fn(u32, &Contributor) -> bool) {
        for (action, state) in &mut self.actions {
            state.pulses.retain_mut(|pulse| {
                pulse.contributors.retain(|c| allowed(*action, c));
                if !pulse.contributors.is_empty() {
                    return true;
                }
                self.pulse_count -= 1;
                false
            });
            state.physically_down = state.pulses.back().is_some_and(|p| p.up_time.is_none());
        }
    }

    pub fn cancel_action(&mut self, action: u32) {
        if let Some(state) = self.actions.remove(&action) {
            self.pulse_count -= state.pulses.len();
        }
    }
}
