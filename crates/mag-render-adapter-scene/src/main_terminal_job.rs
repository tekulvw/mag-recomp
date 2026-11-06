//! Main's CPU reference effects followed by its ring transport.
//!
//! For a pass with draw lists enabled, the job input names two reference words:
//! one is incremented, and the incremented value is then stored into the other.
//! The incremented value is also the reference operand of the transport
//! segment ([`crate::main_terminal_writer`]). This job performs both atomics
//! with real compare-exchange, only after the ring reuse wait, and never
//! replays them.
//!
//! It is usable only once the host owns the exact source for a complete native
//! Main finalization at the real ring state. It does not discharge the pass's
//! other finish effects, CPU dependency words, GPU terminal or presentation.
use crate::{
    main_terminal_writer::{
        CasResult, IoError, PublishedTerminal, TerminalIo, TerminalWriter, WriterError,
    },
    material_finish::OwnedMainFinish,
};
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub enum JobError {
    Input,
    Failed,
    Writer(WriterError),
    Io(IoError),
}
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub struct PublishedCpuTransport {
    terminal: PublishedTerminal,
    reference: Option<u32>,
}
impl PublishedCpuTransport {
    pub fn terminal(self) -> PublishedTerminal {
        self.terminal
    }
    pub fn reference_value(self) -> Option<u32> {
        self.reference
    }
}
enum Stage {
    Reuse,
    IncrementRead,
    IncrementCas,
    ExchangeRead,
    ExchangeCas,
    Transport,
    Done(PublishedCpuTransport),
    Failed,
}
pub struct TerminalJob {
    finish: OwnedMainFinish,
    writer: TerminalWriter,
    stage: Stage,
    observed: u32,
    reference: Option<u32>,
    effects: bool,
}
impl TerminalJob {
    pub fn prepare(finish: OwnedMainFinish) -> Result<Self, JobError> {
        if finish.lists_enabled
            && [
                finish.source_words.reference_increment_address,
                finish.source_words.reference_exchange_address,
            ]
            .iter()
            .any(|a| *a == 0 || !a.is_multiple_of(4) || a.checked_add(4).is_none())
        {
            return Err(JobError::Input);
        }
        // This first construction performs every transport admission check before a
        // reference increment can become externally visible. The operand is replaced
        // by the actual successful compare-exchange result, never an old or supplied
        // ticket.
        let writer = TerminalWriter::prepare(&finish, finish.lists_enabled.then_some(0))
            .map_err(JobError::Writer)?;
        Ok(Self {
            finish,
            writer,
            stage: Stage::Reuse,
            observed: 0,
            reference: None,
            effects: false,
        })
    }
    pub fn effects_possible(&self) -> bool {
        self.effects || self.writer.effects_possible()
    }
    pub fn drive(
        &mut self,
        io: &mut impl TerminalIo,
        steps: usize,
    ) -> Result<Option<PublishedCpuTransport>, JobError> {
        for _ in 0..steps {
            if matches!(self.stage, Stage::Failed) {
                return Err(JobError::Failed);
            }
            match self.step(io) {
                Ok(Some(out)) => return Ok(Some(out)),
                Ok(None) if matches!(self.stage, Stage::Reuse) => return Ok(None),
                Ok(None) => {}
                Err(e) => {
                    self.stage = Stage::Failed;
                    return Err(e);
                }
            }
        }
        Ok(None)
    }
    fn step(
        &mut self,
        io: &mut impl TerminalIo,
    ) -> Result<Option<PublishedCpuTransport>, JobError> {
        let s = self.finish.source_words;
        match self.stage {
            Stage::Reuse => {
                let value = io
                    .read_word(self.finish.writer.reference_address)
                    .map_err(JobError::Io)?;
                if !self.writer.reuse_ready(value) {
                    return Ok(None);
                }
                self.stage = if self.finish.lists_enabled {
                    Stage::IncrementRead
                } else {
                    Stage::Transport
                };
            }
            Stage::IncrementRead => {
                self.observed = io
                    .read_word(s.reference_increment_address)
                    .map_err(JobError::Io)?;
                self.stage = Stage::IncrementCas;
            }
            Stage::IncrementCas => {
                let value = self.observed.wrapping_add(1);
                let previous_effects = self.effects;
                self.effects = true;
                let CasResult { observed, stored } = io
                    .compare_exchange(s.reference_increment_address, self.observed, value)
                    .map_err(JobError::Io)?;
                if stored {
                    self.effects = true;
                    if observed != self.observed {
                        return Err(JobError::Input);
                    }
                    self.reference = Some(value);
                    self.writer = TerminalWriter::prepare(&self.finish, Some(value))
                        .map_err(JobError::Writer)?;
                    crate::main_terminal_writer::count_auxiliary_flush(
                        self.writer.auxiliary_flush(),
                    );
                    self.stage = Stage::ExchangeRead;
                } else {
                    self.effects = previous_effects;
                    self.observed = observed;
                }
            }
            Stage::ExchangeRead => {
                self.observed = io
                    .read_word(s.reference_exchange_address)
                    .map_err(JobError::Io)?;
                self.stage = Stage::ExchangeCas;
            }
            Stage::ExchangeCas => {
                let value = self.reference.ok_or(JobError::Input)?;
                let previous_effects = self.effects;
                self.effects = true;
                let CasResult { observed, stored } = io
                    .compare_exchange(s.reference_exchange_address, self.observed, value)
                    .map_err(JobError::Io)?;
                if stored {
                    self.effects = true;
                    if observed != self.observed {
                        return Err(JobError::Input);
                    }
                    self.stage = Stage::Transport;
                } else {
                    self.effects = previous_effects;
                    self.observed = observed;
                }
            }
            Stage::Transport => {
                if let Some(terminal) = self.writer.drive(io, 1).map_err(JobError::Writer)? {
                    let out = PublishedCpuTransport {
                        terminal,
                        reference: self.reference,
                    };
                    self.stage = Stage::Done(out);
                    return Ok(Some(out));
                }
            }
            Stage::Done(out) => return Ok(Some(out)),
            Stage::Failed => return Err(JobError::Failed),
        }
        Ok(None)
    }
}
