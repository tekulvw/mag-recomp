//! Physical input lifetime is independent of a game's output capabilities.

use std::collections::{BTreeMap, BTreeSet};
use std::fmt;

pub use crate::bindings::{Behavior, Binding};
use crate::delivery::{Contributor, Delivery, Offer};
use crate::input::DeviceControl;
pub use crate::input::{
    Capabilities, Capture, Context, ContextKey, Control, Event, Frame, FrameKey, MouseMotion,
    Source,
};

#[derive(Clone, Copy, Debug, Eq, PartialEq)]
pub enum Error {
    InvalidBinding,
    InvalidFrame,
    StaleGeneration,
    CapacityExceeded,
    InvalidMotion,
    Delivery(crate::delivery::Error),
}

impl fmt::Display for Error {
    fn fmt(&self, output: &mut fmt::Formatter<'_>) -> fmt::Result {
        write!(output, "{self:?}")
    }
}
impl std::error::Error for Error {}
impl From<crate::delivery::Error> for Error {
    fn from(error: crate::delivery::Error) -> Self {
        Self::Delivery(error)
    }
}

pub struct Engine {
    session: u64,
    bindings: Vec<Binding>,
    capture: Capture,
    generation: u64,
    physical: BTreeSet<DeviceControl>,
    blocked: BTreeSet<DeviceControl>,
    accepted: BTreeSet<DeviceControl>,
    toggled: BTreeSet<DeviceControl>,
    pending: Vec<Event>,
    logical: BTreeMap<u32, BTreeSet<Contributor>>,
    delivery: Delivery,
    mouse: MouseMotion,
    context: Option<ContextKey>,
    frame: Option<Frame>,
    last_sequence: u64,
    last_time: Option<u64>,
    sampled: Option<(FrameKey, Vec<Event>, MouseMotion)>,
}

impl Engine {
    pub fn new(session: u64, bindings: Vec<Binding>) -> Result<Self, Error> {
        if session == 0 || !crate::bindings::valid(&bindings) {
            return Err(Error::InvalidBinding);
        }
        Ok(Self {
            session,
            bindings,
            capture: Capture::default(),
            generation: 1,
            physical: BTreeSet::new(),
            blocked: BTreeSet::new(),
            accepted: BTreeSet::new(),
            toggled: BTreeSet::new(),
            pending: Vec::new(),
            logical: BTreeMap::new(),
            delivery: Delivery::default(),
            mouse: MouseMotion::default(),
            context: None,
            frame: None,
            last_sequence: 0,
            last_time: None,
            sampled: None,
        })
    }

    pub fn generation(&self) -> u64 {
        self.generation
    }

    pub fn current_frame(&self, key: FrameKey) -> Result<&Frame, Error> {
        self.frame
            .as_ref()
            .filter(|frame| frame.key == key && frame.generation == self.generation)
            .ok_or(Error::InvalidFrame)
    }

    pub fn reset(&mut self, source: Option<Source>) -> Result<(), Error> {
        match source {
            Some(source) => self.cancel_source(source, None),
            None => self.cancel_all(),
        }
    }

    /// Replace bindings atomically, requiring held controls to be released first.
    pub fn configure(&mut self, bindings: Vec<Binding>) -> Result<(), Error> {
        if !crate::bindings::valid(&bindings) {
            return Err(Error::InvalidBinding);
        }
        self.cancel_all()?;
        self.bindings = bindings;
        Ok(())
    }

    /// End receipt acceptance for a frame without acknowledging its offers.
    pub fn retire(&mut self, key: FrameKey) -> Result<(), Error> {
        if key.session != self.session || key.sequence == 0 || key.player_slot != 0 {
            return Err(Error::InvalidFrame);
        }
        if self.sampled.as_ref().is_some_and(|sample| sample.0 == key) {
            self.cancel_all()?;
            self.last_sequence = key.sequence;
            return Ok(());
        }
        if !self.frame.as_ref().is_some_and(|frame| frame.key == key) {
            return if key.sequence <= self.last_sequence {
                Ok(())
            } else {
                Err(Error::InvalidFrame)
            };
        }
        self.frame = None;
        Ok(())
    }

    /// Freeze ingress at a physical poll; interpretation may occur later.
    pub fn sample(&mut self, key: FrameKey) -> Result<(), Error> {
        if self.sampled.as_ref().is_some_and(|sample| sample.0 == key)
            || self.frame.as_ref().is_some_and(|frame| frame.key == key)
        {
            return Ok(());
        }
        if key.session != self.session
            || key.sequence <= self.last_sequence
            || key.sequence == 0
            || key.player_slot != 0
        {
            return Err(Error::InvalidFrame);
        }
        if let Some((previous, _, _)) = &self.sampled {
            if previous.sequence >= key.sequence {
                return Err(Error::InvalidFrame);
            }
            self.cancel_all()?;
        }
        self.frame = None;
        self.sampled = Some((
            key,
            std::mem::take(&mut self.pending),
            std::mem::take(&mut self.mouse),
        ));
        Ok(())
    }

    fn next_generation(&self) -> Result<u64, Error> {
        self.generation
            .checked_add(1)
            .ok_or(Error::CapacityExceeded)
    }

    fn cancel_all(&mut self) -> Result<(), Error> {
        let generation = self.next_generation()?;
        if let Some((key, _, _)) = &self.sampled {
            self.last_sequence = self.last_sequence.max(key.sequence);
        }
        self.blocked.extend(self.physical.iter().copied());
        self.accepted.clear();
        self.toggled.clear();
        self.pending.clear();
        self.logical.clear();
        self.delivery.cancel();
        self.mouse = MouseMotion::default();
        self.frame = None;
        self.sampled = None;
        self.generation = generation;
        Ok(())
    }

    pub fn set_capture(&mut self, capture: Capture) -> Result<u64, Error> {
        if (self.capture.focused && !capture.focused)
            || (self.capture.supported_context && !capture.supported_context)
        {
            self.cancel_all()?;
        } else {
            if self.capture.mouse_captured && !capture.mouse_captured {
                self.cancel_source(Source::Mouse, None)?;
            }
            if self.capture.controller_connected && !capture.controller_connected {
                self.cancel_source(Source::Controller, None)?;
            }
        }
        self.capture = capture;
        Ok(self.generation)
    }

    pub fn push(&mut self, event: Event) -> Result<(), Error> {
        if event.generation != self.generation {
            return Err(Error::StaleGeneration);
        }
        let control = event.device_control();
        if self.physical.contains(&control) == event.down {
            return Ok(());
        }
        if self.pending.len() >= 8192 || (event.down && self.physical.len() >= 4096) {
            return Err(Error::CapacityExceeded);
        }
        if event.down {
            self.physical.insert(control);
        } else {
            self.physical.remove(&control);
        }
        if self.blocked.contains(&control) {
            if !event.down {
                self.blocked.remove(&control);
            }
            return Ok(());
        }
        if !self.capture.active()
            || (event.control.source == Source::Controller && !self.capture.controller_connected)
        {
            if event.down {
                self.blocked.insert(control);
            }
            return Ok(());
        }
        self.pending.push(event);
        Ok(())
    }

    pub fn push_mouse(&mut self, generation: u64, dx: f64, dy: f64) -> Result<(), Error> {
        if generation != self.generation {
            return Err(Error::StaleGeneration);
        }
        let next = MouseMotion {
            dx: self.mouse.dx + dx,
            dy: self.mouse.dy + dy,
        };
        if !dx.is_finite() || !dy.is_finite() || !next.dx.is_finite() || !next.dy.is_finite() {
            return Err(Error::InvalidMotion);
        }
        if self.capture.active() && self.capture.mouse_captured {
            self.mouse = next;
        }
        Ok(())
    }

    fn action_contributors(&self, action: u32) -> BTreeSet<Contributor> {
        let Some(context) = self.context else {
            return BTreeSet::new();
        };
        let mut contributors = BTreeSet::new();
        for binding in &self.bindings {
            if binding.action != action || binding.contexts & context.context.bit() == 0 {
                continue;
            }
            let held = match binding.behavior {
                Behavior::Hold => &self.accepted,
                Behavior::Toggle => &self.toggled,
            };
            for control in held
                .iter()
                .filter(|control| control.control == binding.control)
            {
                contributors.insert(Contributor {
                    source: binding.control.source.bit(),
                    device: control.device,
                });
            }
        }
        contributors
    }

    fn update_actions(&mut self, time_ns: u64) -> Result<(), Error> {
        let actions: BTreeSet<_> = self.bindings.iter().map(|binding| binding.action).collect();
        for action in actions {
            let contributors = self.action_contributors(action);
            let previous = self.logical.get(&action).cloned().unwrap_or_default();
            if previous.is_empty() != contributors.is_empty() {
                self.delivery.push_from(
                    action,
                    !contributors.is_empty(),
                    time_ns,
                    contributors.clone(),
                )?;
            } else if previous != contributors && !contributors.is_empty() {
                self.delivery.update_contributors(action, &contributors)?;
            }
            self.logical.insert(action, contributors);
        }
        Ok(())
    }

    fn cancel_source(&mut self, source: Source, device: Option<u64>) -> Result<(), Error> {
        let generation = self.next_generation()?;
        let matches = |control: &DeviceControl| {
            control.control.source == source && device.is_none_or(|id| id == control.device)
        };
        self.blocked.extend(
            self.physical
                .iter()
                .filter(|control| matches(control))
                .copied(),
        );
        self.accepted.retain(|control| !matches(control));
        self.toggled.retain(|control| !matches(control));
        self.pending
            .retain(|event| !matches(&event.device_control()));
        if let Some((_, events, motion)) = &mut self.sampled {
            events.retain(|event| !matches(&event.device_control()));
            if source == Source::Mouse {
                *motion = MouseMotion::default();
            }
        }
        self.delivery.cancel_sources(source.bit(), device);
        let actions: Vec<_> = self.logical.keys().copied().collect();
        for action in actions {
            let contributors = self.action_contributors(action);
            if !contributors.is_empty() {
                self.delivery.update_contributors(action, &contributors)?;
            }
            self.logical.insert(action, contributors);
        }
        if source == Source::Mouse {
            self.mouse = MouseMotion::default();
        }
        self.frame = None;
        self.generation = generation;
        Ok(())
    }

    pub fn disconnect(&mut self, source: Source, device: u64) -> Result<(), Error> {
        self.cancel_source(source, Some(device))?;
        self.physical
            .retain(|control| !(control.control.source == source && control.device == device));
        self.blocked
            .retain(|control| !(control.control.source == source && control.device == device));
        Ok(())
    }

    pub fn seal(
        &mut self,
        key: FrameKey,
        now_ns: u64,
        context: ContextKey,
    ) -> Result<Frame, Error> {
        if let Some(frame) = &self.frame
            && frame.key == key
        {
            return if frame.context == context {
                Ok(frame.clone())
            } else {
                Err(Error::InvalidFrame)
            };
        }
        if key.session != self.session
            || key.sequence == 0
            || key.sequence <= self.last_sequence
            || key.player_slot != 0
            || context.owner == 0
            || self.last_time.is_some_and(|last| now_ns < last)
        {
            return Err(Error::InvalidFrame);
        }
        if self.context.is_some_and(|old| old != context) {
            self.cancel_all()?;
            // The new context still gets its simulation opportunity, with no
            // input inherited from the previous owner or context.
            self.sampled = Some((key, Vec::new(), MouseMotion::default()));
        }
        self.context = Some(context);
        self.sample(key)?;
        let (_, events, motion) = self.sampled.take().ok_or(Error::InvalidFrame)?;
        for event in events {
            let control = event.device_control();
            if event.down {
                self.accepted.insert(control);
                if self.bindings.iter().any(|binding| {
                    binding.control == event.control
                        && binding.behavior == Behavior::Toggle
                        && binding.contexts & context.context.bit() != 0
                }) && !self.toggled.remove(&control)
                {
                    self.toggled.insert(control);
                }
            } else {
                self.accepted.remove(&control);
            }
            if let Err(error) = self.update_actions(event.time_ns) {
                self.cancel_all()?;
                return Err(error);
            }
        }
        if let Err(error) = self.delivery.advance(now_ns) {
            self.cancel_all()?;
            return Err(error.into());
        }
        let actions = self
            .logical
            .keys()
            .filter_map(|&action| self.delivery.offer(action).map(|offer| (action, offer)))
            .collect();
        let frame = Frame {
            key,
            time_ns: now_ns,
            generation: self.generation,
            context,
            actions,
            mouse: motion,
            look_allowed: self.capture.active() && self.capture.mouse_captured,
        };
        self.last_sequence = key.sequence;
        self.last_time = Some(now_ns);
        self.frame = Some(frame.clone());
        Ok(frame)
    }

    pub fn observe(&mut self, key: FrameKey, action: u32, offer: Offer) -> Result<(), Error> {
        if !self.frame.as_ref().is_some_and(|frame| {
            frame.key == key
                && frame.generation == self.generation
                && frame.action(action) == Some(offer)
        }) {
            return Err(Error::InvalidFrame);
        }
        self.delivery.observe(action, offer.pulse_id, offer.down)?;
        Ok(())
    }
}
