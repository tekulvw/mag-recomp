use mag_render::{accounting::verify_removal, types::*};

mod common;
use common::{events, manifest};
fn assert_rejected(s: &ScopeManifest, e: &[Event], reason: &str) {
    let r = verify_removal(s, e);
    assert!(!r.passed, "false proof: {r:?}");
    assert!(
        r.errors.iter().any(|e| e.contains(reason)),
        "missing {reason:?}: {r:?}"
    );
}
#[test]
fn complete_independent_receipts_pass() {
    let r = verify_removal(&manifest(), &events());
    assert!(r.passed, "{r:?}");
    assert_eq!(r.unique_completed_frames, 1);
}
#[test]
fn missing_item_fails() {
    let mut e = events();
    e.remove(0);
    assert_rejected(&manifest(), &e, "missing outcome");
}
#[test]
fn duplicate_outcome_fails() {
    let mut e = events();
    let mut copy = e[0].clone();
    copy.sequence = 9;
    e.push(copy);
    assert_rejected(&manifest(), &e, "duplicate outcome");
}
#[test]
fn hidden_original_dispatch_fails() {
    let mut e = events();
    if let EventKind::ObserverReceipt {
        original_entries, ..
    } = &mut e[4].event
    {
        *original_entries = 1;
    }
    assert_rejected(&manifest(), &e, "original execution");
}
#[test]
fn late_original_execution_fails() {
    let mut e = events();
    e.push(Event {
        scope_id: "synthetic-window".into(),
        sequence: 19,
        event: EventKind::OriginalEntry {
            observer: manifest().required_observers[2].clone(),
        },
    });
    assert_rejected(&manifest(), &e, "original execution");
}
#[test]
fn unknown_family_fails() {
    let mut e = events();
    if let EventKind::Outcome { key, .. } = &mut e[0].event {
        key.family = "unknown".into();
    }
    assert_rejected(&manifest(), &e, "unknown family");
}
#[test]
fn all_culled_without_visibility_evidence_fails() {
    let mut e = events();
    for item in &mut e {
        if let EventKind::Outcome { outcome, .. } = &mut item.event {
            *outcome = Outcome::Culled {
                evidence_id: "self-issued".into(),
            };
        }
    }
    assert_rejected(&manifest(), &e, "visibility evidence");
}
#[test]
fn all_culled_even_with_evidence_is_not_rendered_frame() {
    let mut s = manifest();
    s.visibility_evidence = s
        .obligations
        .iter()
        .enumerate()
        .map(|(i, key)| VisibilityEvidence {
            evidence_id: format!("source-{i}"),
            key: key.clone(),
        })
        .collect();
    let mut e = events();
    for (i, event) in e[..2].iter_mut().enumerate() {
        if let EventKind::Outcome { outcome, .. } = &mut event.event {
            *outcome = Outcome::Culled {
                evidence_id: format!("source-{i}"),
            };
        }
    }
    assert_rejected(&s, &e, "no replacement work");
}
#[test]
fn incomplete_observer_coverage_fails() {
    let mut e = events();
    e.remove(5);
    assert_rejected(&manifest(), &e, "missing observer");
}
#[test]
fn omitted_route_in_manifest_fails() {
    let mut s = manifest();
    s.required_observers.remove(2);
    assert_rejected(&s, &events(), "route coverage");
}
#[test]
fn observer_loss_or_overflow_fails() {
    for mode in 0..3 {
        let mut e = events();
        if let EventKind::ObserverReceipt {
            lost_records,
            overflow,
            enabled,
            ..
        } = &mut e[3].event
        {
            match mode {
                0 => *lost_records = 1,
                1 => *overflow = true,
                _ => *enabled = false,
            }
        }
        assert_rejected(&manifest(), &e, "observer integrity");
    }
}
#[test]
fn incomplete_drain_fails() {
    let mut e = events();
    if let EventKind::ObserverReceipt {
        drained_through, ..
    } = &mut e[3].event
    {
        *drained_through = 10;
    }
    assert_rejected(&manifest(), &e, "observer coverage");
}
#[test]
fn repeat_present_is_not_new_frame() {
    let mut e = events();
    e.push(Event {
        scope_id: "synthetic-window".into(),
        sequence: 9,
        event: EventKind::Present {
            frame_id: FrameId(1),
            submission_id: 7,
            present_id: 4,
        },
    });
    let r = verify_removal(&manifest(), &e);
    assert!(r.passed, "{r:?}");
    assert_eq!(r.unique_completed_frames, 1);
    assert_eq!(r.presentations, 2);
    assert_eq!(r.repeated_presentations, 1);
}
#[test]
fn renamed_old_frame_fails() {
    let mut s = manifest();
    let mut old = s.frames[0].clone();
    old.frame_id = FrameId(2);
    s.frames.push(old);
    assert_rejected(&s, &events(), "reused publication");
}
#[test]
fn false_completion_fails() {
    let mut e = events();
    if let EventKind::GpuCompletion { succeeded, .. } = &mut e[2].event {
        *succeeded = false;
    }
    assert_rejected(&manifest(), &e, "completion");
}
#[test]
fn mismatched_completion_identity_fails() {
    let mut e = events();
    if let EventKind::GpuCompletion { frame, .. } = &mut e[2].event {
        frame.source_publication = 101;
    }
    assert_rejected(&manifest(), &e, "completion identity");
}
#[test]
fn empty_scope_and_events_fail() {
    let mut s = manifest();
    s.frames.clear();
    s.obligations.clear();
    s.required_observers.clear();
    assert_rejected(&s, &[], "empty");
}
#[test]
fn fallback_and_failure_cannot_prove_removal() {
    for outcome in [Outcome::OriginalFallback, Outcome::Failure] {
        let mut e = events();
        if let EventKind::Outcome {
            outcome: result, ..
        } = &mut e[0].event
        {
            *result = outcome;
        }
        assert_rejected(&manifest(), &e, "nonreplacement outcome");
    }
}

#[test]
fn completion_cannot_precede_replacement_output() {
    let mut e = events();
    e.swap(0, 2);
    for (i, event) in e.iter_mut().enumerate() {
        event.sequence = i as u64 + 1;
    }
    assert_rejected(&manifest(), &e, "completion precedes replacement");
}
#[test]
fn presentation_cannot_precede_completion() {
    let mut e = events();
    e.swap(2, 7);
    for (i, event) in e.iter_mut().enumerate() {
        event.sequence = i as u64 + 1;
    }
    assert_rejected(&manifest(), &e, "presentation precedes completion");
}
#[test]
fn source_bound_partial_cull_passes_with_real_remaining_work() {
    let mut s = manifest();
    s.visibility_evidence.push(VisibilityEvidence {
        evidence_id: "upstream-hidden".into(),
        key: s.obligations[1].clone(),
    });
    let mut e = events();
    if let EventKind::Outcome { outcome, .. } = &mut e[1].event {
        *outcome = Outcome::Culled {
            evidence_id: "upstream-hidden".into(),
        };
    }
    let r = verify_removal(&s, &e);
    assert!(r.passed, "{r:?}");
    assert_eq!(r.unique_completed_frames, 1);
}
#[test]
fn wrong_window_or_reordered_event_cannot_be_reconciled() {
    for mode in 0..2 {
        let mut e = events();
        if mode == 0 {
            e[0].scope_id = "another-window".into();
        } else {
            e[1].sequence = 1;
        }
        assert_rejected(&manifest(), &e, "event identity/order");
    }
}
