//! Bounded source-pinned synchronous helper-input publication.
//! The production bridge observes the real call; synthetic event replay tests
//! validate this protocol and never establish that a title call occurred.
use crate::{
    configuration::ConfigurationObserver,
    source::ShadowCornerSources,
    views::{ShadowCornerOutput, ViewMathError, prepare_shadow_corners},
};
#[repr(C)]
#[derive(Clone, Copy, Debug)]
pub struct ShadowCallInput {
    pub abi_version: u32,
    pub struct_bytes: u32,
    pub reserved: [u32; 2],
    pub vm: u64,
    pub initialization: u64,
    pub context: u64,
    pub owner: u64,
    pub invocation: u64,
    pub manager: u32,
    pub view: u32,
    pub state_address: u32,
    pub state_value: u32,
    pub parameters: [u32; 3],
    pub pose_rows: [[u32; 4]; 4],
    pub view_scalar_60: u32,
    pub view_scalar_a24: u32,
    pub reserved_tail: u32,
}
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub enum ShadowCallError {
    Invalid,
    Stale,
    NotReady,
    Numeric(ViewMathError),
}
#[derive(Clone, Debug)]
pub struct ObservedShadowCall {
    identity: ShadowCallInput,
    inputs: ShadowCornerSources,
    output: ShadowCornerOutput,
}
impl ObservedShadowCall {
    pub fn invocation(&self) -> u64 {
        self.identity.invocation
    }
    pub fn inputs(&self) -> &ShadowCornerSources {
        &self.inputs
    }
    pub fn output(&self) -> &ShadowCornerOutput {
        &self.output
    }
}
#[derive(Default)]
pub struct ShadowCallObserver {
    vm: u64,
    last_invocation: u64,
}
impl ShadowCallObserver {
    /// Requires the currently live configuration observer, not a detached old
    /// snapshot or caller-supplied integer readiness certificate. The bridge
    /// additionally authenticates entry, source ranges and synchronous ownership.
    pub fn prepare(
        &mut self,
        event: ShadowCallInput,
        configurations: &ConfigurationObserver,
    ) -> Result<ObservedShadowCall, ShadowCallError> {
        if event.abi_version != 1
            || event.struct_bytes as usize != size_of::<ShadowCallInput>()
            || event.reserved != [0; 2]
            || event.reserved_tail != 0
            || event.context == 0
            || event.invocation == 0
            || event.manager < 0x10000
            || event.view < 0x10000
            || event.manager & 15 != 0
            || event.view & 15 != 0
        {
            return Err(ShadowCallError::Invalid);
        }
        let Some(config) = configurations.configuration(event.vm, event.manager) else {
            return Err(ShadowCallError::NotReady);
        };
        if event.initialization != config.initialization_invocation
            || event.state_address != config.state_address
            || event.state_value != 2
            || event.parameters != config.parameters
        {
            return Err(ShadowCallError::NotReady);
        }
        if event.vm < self.vm || (event.vm == self.vm && event.invocation <= self.last_invocation) {
            return Err(ShadowCallError::Stale);
        }
        self.vm = event.vm;
        self.last_invocation = event.invocation;
        let inputs = ShadowCornerSources {
            parameters: event.parameters,
            pose_rows: event.pose_rows,
            view_scalar_60: event.view_scalar_60,
            view_scalar_a24: event.view_scalar_a24,
        };
        let output = prepare_shadow_corners(&inputs).map_err(ShadowCallError::Numeric)?;
        Ok(ObservedShadowCall {
            identity: event,
            inputs,
            output,
        })
    }
}
