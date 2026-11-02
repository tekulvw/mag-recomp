//! Synthetic ownership tests: the scripted receipt source executes no GPU work.
use mag_render::{backend::*, resources::*, types::*};
use std::{
    collections::{HashMap, VecDeque},
    sync::{Arc, Mutex},
};
#[derive(Default)]
struct Script {
    dispositions: VecDeque<SubmitDisposition>,
    events: VecDeque<BackendEvent>,
    poll_error: Option<DevicePollError>,
    panic_submit: bool,
    uploads: HashMap<ResourceCacheKey, Vec<u8>>,
}
struct Device(Arc<Mutex<Script>>);
impl RenderDevice for Device {
    type Commands = u32;
    fn submit(
        &mut self,
        _: SubmissionTicket,
        _: &u32,
        resources: &[ResourceLease],
    ) -> SubmitDisposition {
        assert!(!resources.is_empty());
        let mut script = self.0.lock().unwrap();
        if script.panic_submit {
            script.panic_submit = false;
            drop(script);
            panic!("synthetic driver panic after uncertain commit");
        }
        for resource in resources {
            script
                .uploads
                .entry(resource.cache_key())
                .or_insert_with(|| resource.bytes().to_vec());
        }
        script
            .dispositions
            .pop_front()
            .unwrap_or(SubmitDisposition::Accepted)
    }
    fn poll(&mut self, _: usize) -> Result<Vec<BackendEvent>, DevicePollError> {
        let mut script = self.0.lock().unwrap();
        if let Some(error) = script.poll_error.take() {
            return Err(error);
        }
        Ok(script.events.drain(..).collect())
    }
}
fn frame() -> FrameIdentity {
    FrameIdentity {
        frame_id: FrameId(1),
        world_update_id: WorldUpdateId(1),
        source_publication: 1,
        camera_publication: 1,
    }
}
fn fixture() -> (
    ResourceStore,
    ResourceVersion,
    RenderBackend<Device>,
    Arc<Mutex<Script>>,
) {
    let mut store = ResourceStore::new(ResourceLimits {
        max_resources: 1,
        max_bytes: 4,
    })
    .unwrap();
    let version = store
        .apply(ResourceDelta::Create {
            bytes: vec![1, 2, 3, 4],
        })
        .unwrap();
    let control = Arc::new(Mutex::new(Script::default()));
    let backend = RenderBackend::new(
        Device(control.clone()),
        BackendLimits {
            max_leases_per_submission: 4,
            max_bytes_per_submission: 16,
        },
    )
    .unwrap();
    (store, version, backend, control)
}
fn event(control: &Arc<Mutex<Script>>, event: BackendEvent) {
    control.lock().unwrap().events.push_back(event);
}
fn terminal(control: &Arc<Mutex<Script>>, ticket: SubmissionTicket) {
    event(
        control,
        BackendEvent::Terminal {
            ticket,
            status: TerminalStatus::Succeeded,
        },
    );
}
#[test]
fn delayed_completion_retains_resource_until_actual_terminal() {
    let (mut store, v, mut backend, control) = fixture();
    let ticket = backend
        .submit(frame(), 7, vec![store.lease(v).unwrap()])
        .unwrap();
    store.retire(v).unwrap();
    assert_eq!(backend.pending_count(), 1);
    assert_eq!(store.stats().resident_bytes, 4);
    assert!(backend.poll().unwrap().events.is_empty());
    assert_eq!(store.stats().resident_bytes, 4);
    terminal(&control, ticket);
    assert_eq!(backend.poll().unwrap().events.len(), 1);
    assert_eq!(backend.pending_count(), 0);
    assert_eq!(store.stats().resident_bytes, 0);
}
#[test]
fn readiness_terminal_and_present_are_distinct() {
    let (mut store, v, mut backend, control) = fixture();
    let ticket = backend
        .submit(frame(), 7, vec![store.lease(v).unwrap()])
        .unwrap();
    store.retire(v).unwrap();
    event(&control, BackendEvent::SourceOutputsReady { ticket });
    assert!(backend.poll().unwrap().errors.is_empty());
    let s = backend.state(ticket).unwrap();
    assert!(s.source_outputs_ready);
    assert_eq!(s.gpu_terminal, None);
    assert_eq!(s.presentations, 0);
    assert_eq!(store.stats().resident_bytes, 4);
    terminal(&control, ticket);
    backend.poll().unwrap();
    let s = backend.state(ticket).unwrap();
    assert_eq!(s.gpu_terminal, Some(TerminalStatus::Succeeded));
    assert_eq!(s.presentations, 0);
    event(
        &control,
        BackendEvent::Presented {
            ticket,
            presentation_id: 11,
        },
    );
    backend.poll().unwrap();
    assert_eq!(backend.state(ticket).unwrap().presentations, 1);
}
#[test]
fn two_pending_limit_and_no_early_slot_reuse() {
    let (mut store, v, mut backend, control) = fixture();
    let a = backend
        .submit(frame(), 7, vec![store.lease(v).unwrap()])
        .unwrap();
    let b = backend
        .submit(frame(), 7, vec![store.lease(v).unwrap()])
        .unwrap();
    assert_eq!(
        backend.submit(frame(), 7, vec![store.lease(v).unwrap()]),
        Err(BackendError::PendingLimit)
    );
    store.retire(v).unwrap();
    terminal(&control, a);
    backend.poll().unwrap();
    assert_eq!(
        store.apply(ResourceDelta::Create { bytes: vec![9] }),
        Err(ResourceError::CapacityExceeded)
    );
    terminal(&control, b);
    backend.poll().unwrap();
    let new = store
        .apply(ResourceDelta::Create { bytes: vec![9] })
        .unwrap();
    assert_eq!(new.id.generation, v.id.generation + 1);
}
#[test]
fn definitely_rejected_submit_releases_lease_but_uncertain_does_not() {
    let (mut store, v, mut backend, control) = fixture();
    control
        .lock()
        .unwrap()
        .dispositions
        .push_back(SubmitDisposition::DefinitelyRejected { code: 5 });
    assert_eq!(
        backend.submit(frame(), 7, vec![store.lease(v).unwrap()]),
        Err(BackendError::DeviceRejected { code: 5 })
    );
    assert_eq!(backend.pending_count(), 0);
    control
        .lock()
        .unwrap()
        .dispositions
        .push_back(SubmitDisposition::Indeterminate { code: 6 });
    let error = backend
        .submit(frame(), 7, vec![store.lease(v).unwrap()])
        .unwrap_err();
    let ticket = match error {
        BackendError::SubmissionUncertain { ticket, code: 6 } => ticket,
        _ => panic!("{error:?}"),
    };
    store.retire(v).unwrap();
    assert_eq!(store.stats().resident_bytes, 4);
    assert_eq!(
        backend.submit(frame(), 7, vec![]),
        Err(BackendError::Faulted)
    );
    event(
        &control,
        BackendEvent::Terminal {
            ticket,
            status: TerminalStatus::Failed { code: 6 },
        },
    );
    backend.poll().unwrap();
    assert_eq!(store.stats().resident_bytes, 0);
}
#[test]
fn poll_device_loss_never_implies_terminal_completion() {
    let (mut store, v, mut backend, control) = fixture();
    let ticket = backend
        .submit(frame(), 7, vec![store.lease(v).unwrap()])
        .unwrap();
    store.retire(v).unwrap();
    control.lock().unwrap().poll_error = Some(DevicePollError {
        code: 99,
        device_lost: true,
    });
    assert_eq!(
        backend.poll().unwrap_err(),
        BackendError::DevicePoll { code: 99 }
    );
    assert_eq!(backend.pending_count(), 1);
    assert_eq!(store.stats().resident_bytes, 4);
    event(
        &control,
        BackendEvent::Terminal {
            ticket,
            status: TerminalStatus::Failed { code: 99 },
        },
    );
    backend.poll().unwrap();
    assert_eq!(store.stats().resident_bytes, 0);
}
#[test]
fn duplicate_and_wrong_context_completion_rejected_without_freeing_other_work() {
    let (mut store, v, mut backend, control) = fixture();
    let (_, _, other, _) = fixture();
    let a = backend
        .submit(frame(), 7, vec![store.lease(v).unwrap()])
        .unwrap();
    let b = backend
        .submit(frame(), 7, vec![store.lease(v).unwrap()])
        .unwrap();
    store.retire(v).unwrap();
    terminal(
        &control,
        SubmissionTicket {
            context: other.context(),
            submission_id: a.submission_id,
        },
    );
    assert_eq!(backend.poll().unwrap().errors, [BackendError::WrongContext]);
    assert_eq!(backend.pending_count(), 2);
    terminal(&control, a);
    backend.poll().unwrap();
    terminal(&control, a);
    assert_eq!(
        backend.poll().unwrap().errors,
        [BackendError::DuplicateEvent]
    );
    assert_eq!(store.stats().resident_bytes, 4);
    terminal(&control, b);
    backend.poll().unwrap();
    assert_eq!(store.stats().resident_bytes, 0);
}
#[test]
fn shutdown_and_resize_refuse_unresolved_work_and_stale_events() {
    let (store, v, mut backend, control) = fixture();
    let ticket = backend
        .submit(frame(), 7, vec![store.lease(v).unwrap()])
        .unwrap();
    assert_eq!(backend.try_shutdown(), Err(BackendError::PendingWork));
    assert_eq!(backend.advance_generation(), Err(BackendError::PendingWork));
    terminal(&control, ticket);
    backend.poll().unwrap();
    let next = backend.advance_generation().unwrap();
    assert_ne!(next, ticket.context);
    terminal(&control, ticket);
    assert_eq!(backend.poll().unwrap().errors, [BackendError::WrongContext]);
    backend.try_shutdown().unwrap();
    assert_eq!(
        backend.submit(frame(), 7, vec![store.lease(v).unwrap()]),
        Err(BackendError::Closed)
    );
}
#[test]
fn invalid_present_does_not_release_pending_and_batch_still_drains_terminal() {
    let (mut store, v, mut backend, control) = fixture();
    let ticket = backend
        .submit(frame(), 7, vec![store.lease(v).unwrap()])
        .unwrap();
    store.retire(v).unwrap();
    event(
        &control,
        BackendEvent::Presented {
            ticket,
            presentation_id: 1,
        },
    );
    terminal(&control, ticket);
    let r = backend.poll().unwrap();
    assert_eq!(r.errors, [BackendError::InvalidEventOrder]);
    assert_eq!(r.events.len(), 1);
    assert_eq!(store.stats().resident_bytes, 0);
}
#[test]
fn driver_panic_keeps_uncertain_submission_owned() {
    let (mut store, v, mut backend, control) = fixture();
    control.lock().unwrap().panic_submit = true;
    let error = backend
        .submit(frame(), 7, vec![store.lease(v).unwrap()])
        .unwrap_err();
    let ticket = match error {
        BackendError::SubmissionUncertain { ticket, .. } => ticket,
        _ => panic!("{error:?}"),
    };
    store.retire(v).unwrap();
    assert_eq!(store.stats().resident_bytes, 4);
    event(
        &control,
        BackendEvent::Terminal {
            ticket,
            status: TerminalStatus::Failed { code: 1 },
        },
    );
    backend.poll().unwrap();
    assert_eq!(store.stats().resident_bytes, 0);
}
#[test]
fn unexpected_drop_retains_unresolved_leases_as_documented_failure_leak() {
    let (mut store, v, mut backend, _) = fixture();
    backend
        .submit(frame(), 7, vec![store.lease(v).unwrap()])
        .unwrap();
    store.retire(v).unwrap();
    drop(backend);
    // Intentional four-byte failure-path retention, never fabricated completion.
    assert_eq!(store.stats().resident_bytes, 4);
}
#[test]
fn contexts_minted_by_backend_cannot_be_reused_by_caller() {
    let (_, _, a, _) = fixture();
    let (_, _, b, _) = fixture();
    assert_ne!(a.context(), b.context());
}

#[test]
fn invalid_frame_and_lease_budgets_decline_before_driver_effects() {
    let (store, v, mut backend, control) = fixture();
    control
        .lock()
        .unwrap()
        .dispositions
        .push_back(SubmitDisposition::DefinitelyRejected { code: 42 });
    let mut invalid = frame();
    invalid.source_publication = 0;
    assert_eq!(
        backend.submit(invalid, 7, vec![store.lease(v).unwrap()]),
        Err(BackendError::InvalidFrame)
    );
    assert_eq!(
        backend.submit(frame(), 7, vec![]),
        Err(BackendError::LeaseLimit)
    );
    assert_eq!(
        backend.submit(
            frame(),
            7,
            (0..5).map(|_| store.lease(v).unwrap()).collect()
        ),
        Err(BackendError::LeaseLimit)
    );
    assert_eq!(backend.pending_count(), 0);
    // The first device disposition remains unused until this valid submission.
    assert_eq!(
        backend.submit(frame(), 7, vec![store.lease(v).unwrap()]),
        Err(BackendError::DeviceRejected { code: 42 })
    );
    let control = Arc::new(Mutex::new(Script::default()));
    let mut backend = RenderBackend::new(
        Device(control),
        BackendLimits {
            max_leases_per_submission: 4,
            max_bytes_per_submission: 3,
        },
    )
    .unwrap();
    assert_eq!(
        backend.submit(frame(), 7, vec![store.lease(v).unwrap()]),
        Err(BackendError::LeaseLimit)
    );
}
#[test]
fn rejected_submission_is_not_a_hidden_lease_owner() {
    let (mut store, v, mut backend, control) = fixture();
    control
        .lock()
        .unwrap()
        .dispositions
        .push_back(SubmitDisposition::DefinitelyRejected { code: 7 });
    assert!(matches!(
        backend.submit(frame(), 7, vec![store.lease(v).unwrap()]),
        Err(BackendError::DeviceRejected { code: 7 })
    ));
    store.retire(v).unwrap();
    assert_eq!(store.stats().resident_bytes, 0);
    backend.try_shutdown().unwrap();
}
#[test]
fn terminal_history_is_bounded_and_unknown_events_cannot_retire_active_ticket() {
    let (mut store, v, mut backend, control) = fixture();
    let mut first = None;
    for _ in 0..65 {
        let t = backend
            .submit(frame(), 7, vec![store.lease(v).unwrap()])
            .unwrap();
        if first.is_none() {
            first = Some(t);
        }
        terminal(&control, t);
        backend.poll().unwrap();
    }
    assert_eq!(
        backend.state(first.unwrap()),
        Err(BackendError::UnknownTicket)
    );
    let active = backend
        .submit(frame(), 7, vec![store.lease(v).unwrap()])
        .unwrap();
    store.retire(v).unwrap();
    terminal(
        &control,
        SubmissionTicket {
            context: backend.context(),
            submission_id: u64::MAX,
        },
    );
    assert_eq!(
        backend.poll().unwrap().errors,
        [BackendError::UnknownTicket]
    );
    assert_eq!(store.stats().resident_bytes, 4);
    terminal(&control, active);
    backend.poll().unwrap();
    assert_eq!(store.stats().resident_bytes, 0);
}
#[test]
fn duplicate_source_ready_and_present_receipts_are_rejected() {
    let (store, v, mut backend, control) = fixture();
    let t = backend
        .submit(frame(), 7, vec![store.lease(v).unwrap()])
        .unwrap();
    event(&control, BackendEvent::SourceOutputsReady { ticket: t });
    event(&control, BackendEvent::SourceOutputsReady { ticket: t });
    terminal(&control, t);
    assert_eq!(
        backend.poll().unwrap().errors,
        [BackendError::DuplicateEvent]
    );
    event(
        &control,
        BackendEvent::Presented {
            ticket: t,
            presentation_id: 1,
        },
    );
    event(
        &control,
        BackendEvent::Presented {
            ticket: t,
            presentation_id: 1,
        },
    );
    assert_eq!(
        backend.poll().unwrap().errors,
        [BackendError::DuplicateEvent]
    );
    assert_eq!(backend.state(t).unwrap().presentations, 1);
}
#[test]
fn invalid_limits_and_adapter_poll_overflow_fail_closed() {
    let control = Arc::new(Mutex::new(Script::default()));
    assert!(matches!(
        RenderBackend::new(
            Device(control),
            BackendLimits {
                max_leases_per_submission: 0,
                max_bytes_per_submission: 4
            }
        ),
        Err(BackendError::InvalidLimits)
    ));
    let (mut store, v, mut backend, control) = fixture();
    let t = backend
        .submit(frame(), 7, vec![store.lease(v).unwrap()])
        .unwrap();
    store.retire(v).unwrap();
    for _ in 0..65 {
        event(&control, BackendEvent::SourceOutputsReady { ticket: t });
    }
    assert_eq!(backend.poll().unwrap_err(), BackendError::EventOverflow);
    assert_eq!(store.stats().resident_bytes, 4);
    terminal(&control, t);
    backend.poll().unwrap();
    assert_eq!(store.stats().resident_bytes, 0);
}

#[test]
fn two_store_slot_version_collisions_preserve_distinct_upload_bytes() {
    let (store, a, mut backend, control) = fixture();
    let mut other = ResourceStore::new(ResourceLimits {
        max_resources: 1,
        max_bytes: 4,
    })
    .unwrap();
    let b = other
        .apply(ResourceDelta::Create {
            bytes: vec![9, 8, 7, 6],
        })
        .unwrap();
    assert_eq!(a, b); // Wire identities are deliberately store-local and collide.
    let lease_a = store.lease(a).unwrap();
    let lease_b = other.lease(b).unwrap();
    let key_a = lease_a.cache_key();
    let key_b = lease_b.cache_key();
    let t = backend.submit(frame(), 7, vec![lease_a, lease_b]).unwrap();
    terminal(&control, t);
    backend.poll().unwrap();
    let script = control.lock().unwrap();
    assert_eq!(
        script.uploads.len(),
        2,
        "independent resources aliased in upload cache"
    );
    assert_eq!(script.uploads[&key_a], [1, 2, 3, 4]);
    assert_eq!(script.uploads[&key_b], [9, 8, 7, 6]);
}
