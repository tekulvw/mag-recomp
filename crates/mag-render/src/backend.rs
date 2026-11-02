//! Submission ownership only. Device adapters must supply genuine GPU receipts.
use crate::{resources::ResourceLease, types::FrameIdentity};
use std::{
    collections::VecDeque,
    panic::{AssertUnwindSafe, catch_unwind},
    sync::atomic::{AtomicU64, Ordering},
};
const MAX_PENDING: usize = 2;
const HISTORY_CAPACITY: usize = 64;
const POLL_CAPACITY: usize = 64;
static NEXT_CONTEXT: AtomicU64 = AtomicU64::new(1);

#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub struct BackendContext {
    id: u64,
    generation: u64,
}
impl BackendContext {
    pub fn id(self) -> u64 {
        self.id
    }
    pub fn generation(self) -> u64 {
        self.generation
    }
}
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub struct SubmissionTicket {
    pub context: BackendContext,
    pub submission_id: u64,
}
#[derive(Clone, Copy, Debug)]
pub struct BackendLimits {
    pub max_leases_per_submission: usize,
    pub max_bytes_per_submission: usize,
}
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub enum SubmitDisposition {
    Accepted,
    DefinitelyRejected { code: u32 },
    Indeterminate { code: u32 },
}
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub enum TerminalStatus {
    Succeeded,
    Failed { code: u32 },
}
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub enum BackendEvent {
    SourceOutputsReady {
        ticket: SubmissionTicket,
    },
    Terminal {
        ticket: SubmissionTicket,
        status: TerminalStatus,
    },
    Presented {
        ticket: SubmissionTicket,
        presentation_id: u64,
    },
}
impl BackendEvent {
    pub fn ticket(self) -> SubmissionTicket {
        match self {
            Self::SourceOutputsReady { ticket }
            | Self::Terminal { ticket, .. }
            | Self::Presented { ticket, .. } => ticket,
        }
    }
}
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub struct DevicePollError {
    pub code: u32,
    pub device_lost: bool,
}
/// Trusted native adapter boundary, not a software renderer. `submit` consumes
/// descriptors and borrowed bytes before return; it retains native resources
/// independently. DefinitelyRejected guarantees no GPU access was committed.
/// Indeterminate preserves its ticket and cannot permit original-work replay.
/// Terminal receipts (including failure) guarantee no further GPU access.
/// Poll errors/empty polls provide no completion evidence. Adapters must obey
/// capacity and map their SDK context/generation before returning events.
///
/// GPU upload caches MUST use ResourceLease::cache_key() (store namespace,
/// slot/generation/content version) plus native device/context generation.
/// ResourceVersion alone can collide across independent stores.
/// Resource dirty spans refer only to the preceding content version: a GPU
/// cache that skipped that predecessor must perform a full upload.
pub trait RenderDevice {
    type Commands;
    fn submit(
        &mut self,
        ticket: SubmissionTicket,
        commands: &Self::Commands,
        resources: &[ResourceLease],
    ) -> SubmitDisposition;
    fn poll(&mut self, capacity: usize) -> Result<Vec<BackendEvent>, DevicePollError>;
}
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub enum BackendError {
    InvalidLimits,
    ContextExhausted,
    Closed,
    Faulted,
    InvalidFrame,
    PendingLimit,
    LeaseLimit,
    AllocationFailed,
    EventOverflow,
    UnknownTicket,
    WrongContext,
    DuplicateEvent,
    InvalidEventOrder,
    PendingWork,
    DeviceRejected { code: u32 },
    SubmissionUncertain { ticket: SubmissionTicket, code: u32 },
    DevicePoll { code: u32 },
    DriverPanicked,
}
#[derive(Clone, Debug, PartialEq, Eq)]
pub struct TicketState {
    pub frame: FrameIdentity,
    pub source_outputs_ready: bool,
    pub gpu_terminal: Option<TerminalStatus>,
    pub presentations: u64,
}
#[derive(Debug)]
pub struct PollReport {
    pub events: Vec<BackendEvent>,
    pub errors: Vec<BackendError>,
}
struct Record {
    ticket: SubmissionTicket,
    state: TicketState,
    last_presentation_id: u64,
}
struct Pending {
    record: Record,
    _leases: Box<[ResourceLease]>,
}
/// One backend owner with at most two submissions. A trusted RenderDevice is
/// responsible for real device receipts; this layer never synthesizes them.
pub struct RenderBackend<D: RenderDevice> {
    device: Option<D>,
    context: BackendContext,
    limits: BackendLimits,
    next_submission: u64,
    pending: Vec<Pending>,
    history: VecDeque<Record>,
    closed: bool,
    faulted: bool,
}
impl<D: RenderDevice> RenderBackend<D> {
    pub fn new(device: D, limits: BackendLimits) -> Result<Self, BackendError> {
        if limits.max_leases_per_submission == 0 || limits.max_bytes_per_submission == 0 {
            return Err(BackendError::InvalidLimits);
        }
        let mut pending = Vec::new();
        pending
            .try_reserve_exact(MAX_PENDING)
            .map_err(|_| BackendError::AllocationFailed)?;
        let mut history = VecDeque::new();
        history
            .try_reserve_exact(HISTORY_CAPACITY)
            .map_err(|_| BackendError::AllocationFailed)?;
        let id = NEXT_CONTEXT
            .try_update(Ordering::Relaxed, Ordering::Relaxed, |n| n.checked_add(1))
            .map_err(|_| BackendError::ContextExhausted)?;
        Ok(Self {
            device: Some(device),
            context: BackendContext { id, generation: 1 },
            limits,
            next_submission: 1,
            pending,
            history,
            closed: false,
            faulted: false,
        })
    }
    /// Read-only adapter access for diagnostics/capabilities. Native readback
    /// remains BUSY until actual terminal receipts have been polled.
    pub fn device(&self) -> &D {
        self.device.as_ref().expect("backend retains device owner")
    }
    pub fn context(&self) -> BackendContext {
        self.context
    }
    /// Commands and borrowed lease slices are consumed synchronously by the
    /// adapter. Accepted or uncertain work keeps every lease until terminal.
    pub fn submit(
        &mut self,
        frame: FrameIdentity,
        commands: D::Commands,
        leases: Vec<ResourceLease>,
    ) -> Result<SubmissionTicket, BackendError> {
        if self.closed {
            return Err(BackendError::Closed);
        }
        if self.faulted {
            return Err(BackendError::Faulted);
        }
        if self.pending.len() >= MAX_PENDING {
            return Err(BackendError::PendingLimit);
        }
        if frame.frame_id.0 == 0
            || frame.world_update_id.0 == 0
            || frame.source_publication == 0
            || frame.camera_publication == 0
        {
            return Err(BackendError::InvalidFrame);
        }
        let bytes = leases
            .iter()
            .try_fold(0usize, |sum, lease| sum.checked_add(lease.bytes().len()));
        if leases.is_empty()
            || leases.len() > self.limits.max_leases_per_submission
            || bytes.is_none_or(|n| n > self.limits.max_bytes_per_submission)
        {
            return Err(BackendError::LeaseLimit);
        }
        let submission_id = self.next_submission;
        self.next_submission = submission_id
            .checked_add(1)
            .ok_or(BackendError::ContextExhausted)?;
        let ticket = SubmissionTicket {
            context: self.context,
            submission_id,
        };
        // Install ownership before entering driver code, including a panic or
        // error after the driver may already have committed native work.
        self.pending.push(Pending {
            record: Record {
                ticket,
                state: TicketState {
                    frame,
                    source_outputs_ready: false,
                    gpu_terminal: None,
                    presentations: 0,
                },
                last_presentation_id: 0,
            },
            _leases: leases.into_boxed_slice(),
        });
        let pending = self.pending.last().ok_or(BackendError::UnknownTicket)?;
        let disposition = catch_unwind(AssertUnwindSafe(|| {
            self.device
                .as_mut()
                .expect("open backend owns device")
                .submit(ticket, &commands, &pending._leases)
        }));
        match disposition {
            Ok(SubmitDisposition::Accepted) => Ok(ticket),
            Ok(SubmitDisposition::DefinitelyRejected { code }) => {
                self.pending.pop();
                Err(BackendError::DeviceRejected { code })
            }
            Ok(SubmitDisposition::Indeterminate { code }) => {
                self.faulted = true;
                Err(BackendError::SubmissionUncertain { ticket, code })
            }
            Err(_) => {
                self.faulted = true;
                Err(BackendError::SubmissionUncertain {
                    ticket,
                    code: u32::MAX,
                })
            }
        }
    }
    /// Process every returned receipt, even when another receipt in the batch
    /// is invalid, so a rejected duplicate cannot hide a valid terminal event.
    pub fn poll(&mut self) -> Result<PollReport, BackendError> {
        if self.closed {
            return Err(BackendError::Closed);
        }
        let events = match catch_unwind(AssertUnwindSafe(|| {
            self.device
                .as_mut()
                .expect("open backend owns device")
                .poll(POLL_CAPACITY)
        })) {
            Ok(Ok(events)) => events,
            Ok(Err(error)) => {
                self.faulted |= error.device_lost;
                return Err(BackendError::DevicePoll { code: error.code });
            }
            Err(_) => {
                self.faulted = true;
                return Err(BackendError::DriverPanicked);
            }
        };
        if events.len() > POLL_CAPACITY {
            self.faulted = true;
            return Err(BackendError::EventOverflow);
        }
        let mut report = PollReport {
            events: Vec::new(),
            errors: Vec::new(),
        };
        for event in events {
            match self.process(event) {
                Ok(()) => report.events.push(event),
                Err(error) => {
                    self.faulted = true;
                    report.errors.push(error);
                }
            }
        }
        Ok(report)
    }
    fn process(&mut self, event: BackendEvent) -> Result<(), BackendError> {
        let ticket = event.ticket();
        if ticket.context != self.context {
            return Err(BackendError::WrongContext);
        }
        let pending = self.pending.iter().position(|p| p.record.ticket == ticket);
        let record = if let Some(index) = pending {
            &mut self.pending[index].record
        } else {
            self.history
                .iter_mut()
                .find(|r| r.ticket == ticket)
                .ok_or(BackendError::UnknownTicket)?
        };
        match event {
            BackendEvent::SourceOutputsReady { .. } => {
                if record.state.source_outputs_ready {
                    return Err(BackendError::DuplicateEvent);
                }
                record.state.source_outputs_ready = true;
            }
            BackendEvent::Terminal { status, .. } => {
                if record.state.gpu_terminal.is_some() {
                    return Err(BackendError::DuplicateEvent);
                }
                let index = pending.ok_or(BackendError::UnknownTicket)?;
                record.state.gpu_terminal = Some(status);
                if matches!(status, TerminalStatus::Failed { .. }) {
                    self.faulted = true;
                }
                let completed = self.pending.remove(index);
                if self.history.len() == HISTORY_CAPACITY {
                    self.history.pop_front();
                }
                self.history.push_back(completed.record);
                // completed._leases drops only after genuine no-further-access
                // terminal status supplied by the adapter, including failure.
            }
            BackendEvent::Presented {
                presentation_id, ..
            } => {
                if record.state.gpu_terminal != Some(TerminalStatus::Succeeded) {
                    return Err(BackendError::InvalidEventOrder);
                }
                if presentation_id == 0 || presentation_id <= record.last_presentation_id {
                    return Err(BackendError::DuplicateEvent);
                }
                record.last_presentation_id = presentation_id;
                record.state.presentations = record
                    .state
                    .presentations
                    .checked_add(1)
                    .ok_or(BackendError::EventOverflow)?;
            }
        }
        Ok(())
    }
    pub fn state(&self, ticket: SubmissionTicket) -> Result<TicketState, BackendError> {
        if ticket.context != self.context {
            return Err(BackendError::WrongContext);
        }
        self.pending
            .iter()
            .map(|p| &p.record)
            .chain(self.history.iter())
            .find(|r| r.ticket == ticket)
            .map(|r| r.state.clone())
            .ok_or(BackendError::UnknownTicket)
    }
    pub fn pending_count(&self) -> usize {
        self.pending.len()
    }
    /// Ownership gate only; adapters remain responsible for actual SDK teardown.
    pub fn try_shutdown(&mut self) -> Result<(), BackendError> {
        if self.closed {
            return Err(BackendError::Closed);
        }
        if !self.pending.is_empty() {
            return Err(BackendError::PendingWork);
        }
        self.closed = true;
        Ok(())
    }
    /// Gate context recreation/resize. Call only after actual device recovery;
    /// changing this generation does not recreate a device or a swapchain.
    pub fn advance_generation(&mut self) -> Result<BackendContext, BackendError> {
        if self.closed {
            return Err(BackendError::Closed);
        }
        if !self.pending.is_empty() {
            return Err(BackendError::PendingWork);
        }
        self.context.generation = self
            .context
            .generation
            .checked_add(1)
            .ok_or(BackendError::ContextExhausted)?;
        self.history.clear();
        self.faulted = false;
        Ok(self.context)
    }
}
impl<D: RenderDevice> Drop for RenderBackend<D> {
    fn drop(&mut self) {
        if !self.pending.is_empty() {
            // Destruction cannot report an error. An unexpected unresolved Drop
            // leaks at most two bounded lease batches and the device owner;
            // releasing memory or native objects would fabricate retirement.
            // Normal operation must drain receipts and call try_shutdown.
            for pending in self.pending.drain(..) {
                std::mem::forget(pending);
            }
            if let Some(device) = self.device.take() {
                std::mem::forget(device);
            }
        }
    }
}
