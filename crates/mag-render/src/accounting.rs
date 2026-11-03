//! Reconcile an independent pre-interception denominator against immutable
//! producer, original-entry and backend receipts. A pass is a *synthetic proof
//! of consistency*, not authentication or evidence of real-device execution.
use crate::types::*;
use serde::Serialize;
use std::collections::{BTreeMap, BTreeSet};

#[derive(Debug, Serialize)]
pub struct VerificationReport {
    pub passed: bool,
    pub errors: Vec<String>,
    pub unique_completed_frames: usize,
    pub presentations: usize,
    pub repeated_presentations: usize,
}

pub fn verify_removal(scope: &ScopeManifest, events: &[Event]) -> VerificationReport {
    let mut errors = Vec::new();
    if scope.schema_version != 1
        || scope.scope_id.trim().is_empty()
        || scope.upstream_receipt.trim().is_empty()
    {
        errors.push("invalid manifest identity/version".into());
    }
    if scope.window_start == 0
        || scope.window_start > scope.window_end
        || scope.drained_through <= scope.window_end
    {
        errors.push("invalid window or missing late-event drain".into());
    }
    if scope.obligations.is_empty()
        || scope.frames.is_empty()
        || events.is_empty()
        || scope.required_observers.is_empty()
        || scope.families.is_empty()
    {
        errors.push("empty scope/events cannot prove removal".into());
    }

    let families: BTreeSet<_> = scope.families.iter().collect();
    if families.len() != scope.families.len() || families.iter().any(|f| f.trim().is_empty()) {
        errors.push("invalid/duplicate family inventory".into());
    }
    let frames: BTreeMap<_, _> = scope.frames.iter().map(|f| (f.frame_id, f)).collect();
    if frames.len() != scope.frames.len() {
        errors.push("duplicate frame identity".into());
    }
    let mut publications = BTreeSet::new();
    let mut last_frame = 0;
    let mut last_world = 0;
    for frame in &scope.frames {
        if frame.frame_id.0 <= last_frame
            || frame.world_update_id.0 < last_world
            || frame.world_update_id.0 == 0
            || frame.source_publication == 0
            || frame.camera_publication == 0
        {
            errors.push("invalid/nonmonotonic frame identity".into());
        }
        if !publications.insert((frame.source_publication, frame.camera_publication)) {
            errors.push("reused publication cannot become a new frame".into());
        }
        last_frame = frame.frame_id.0;
        last_world = frame.world_update_id.0;
    }
    let obligations: BTreeSet<_> = scope.obligations.iter().collect();
    if obligations.len() != scope.obligations.len() {
        errors.push("duplicate upstream obligation".into());
    }
    let mut stages = BTreeSet::new();
    for key in &scope.obligations {
        validate_work(key, &families, &frames, &mut errors);
        stages.insert((&key.family, &key.stage));
    }
    for family in &scope.families {
        if !stages.iter().any(|(f, _)| *f == family) {
            errors.push(format!("family has no obligations: {family}"));
        }
    }
    let observers: BTreeSet<_> = scope.required_observers.iter().collect();
    if observers.len() != scope.required_observers.len() {
        errors.push("duplicate observer requirement".into());
    }
    for observer in &scope.required_observers {
        if !families.contains(&observer.family)
            || observer.thread_id == 0
            || !stages.contains(&(&observer.family, &observer.stage))
        {
            errors.push("unknown family/stage/thread in observer requirement".into());
        }
    }
    // A missing entire route must not turn absent logging into a zero counter.
    // This first schema conservatively requires all routes for every stage.
    for (family, stage) in stages {
        for route in [
            Route::PpuEntry,
            Route::SdkLifted,
            Route::SdkInterpreter,
            Route::OutsideTransaction,
        ] {
            if !scope
                .required_observers
                .iter()
                .any(|o| &o.family == family && &o.stage == stage && o.route == route)
            {
                errors.push(format!(
                    "missing route coverage: {family}/{stage}/{route:?}"
                ));
            }
        }
    }
    let mut evidence = BTreeMap::new();
    for item in &scope.visibility_evidence {
        if item.evidence_id.trim().is_empty()
            || !obligations.contains(&item.key)
            || evidence.insert(&item.evidence_id, &item.key).is_some()
        {
            errors.push("invalid/duplicate independent visibility evidence".into());
        }
    }

    let mut outcomes = BTreeMap::new();
    let mut observed = BTreeSet::new();
    let mut completions = BTreeMap::new();
    let mut presents = Vec::new();
    let mut present_ids = BTreeSet::new();
    let mut last_sequence = 0;
    for event in events {
        if event.scope_id != scope.scope_id || event.sequence <= last_sequence {
            errors.push("event identity/order mismatch".into());
        }
        last_sequence = event.sequence;
        match &event.event {
            EventKind::Outcome { key, outcome } => {
                validate_work(key, &families, &frames, &mut errors);
                if !obligations.contains(key) {
                    errors.push("unexpected outcome outside upstream obligations".into());
                }
                if outcomes.insert(key, outcome).is_some() {
                    errors.push("duplicate outcome".into());
                }
                match outcome {
                    Outcome::Replacement { submission_id } if *submission_id != 0 => {
                        if completions.contains_key(submission_id) {
                            errors.push("completion precedes replacement output".into());
                        }
                    }
                    Outcome::Replacement { .. } => {
                        errors.push("invalid replacement submission".into())
                    }
                    Outcome::Culled { evidence_id } => {
                        if evidence.get(evidence_id) != Some(&key) {
                            errors
                                .push("missing/mismatched independent visibility evidence".into());
                        }
                    }
                    Outcome::OriginalFallback | Outcome::Failure => {
                        errors.push("nonreplacement outcome prevents removal proof".into())
                    }
                }
            }
            EventKind::ObserverReceipt {
                observer,
                covered_from,
                drained_through,
                original_entries,
                lost_records,
                overflow,
                enabled,
            } => {
                if !observers.contains(observer) {
                    errors.push("unexpected/unknown observer receipt".into());
                }
                if !observed.insert(observer) {
                    errors.push("duplicate observer receipt".into());
                }
                if *covered_from > scope.window_start || *drained_through < scope.drained_through {
                    errors.push("incomplete observer coverage/drain".into());
                }
                if *lost_records != 0 || *overflow || !*enabled {
                    errors.push("observer integrity failure: loss/overflow/disabled".into());
                }
                if *original_entries != 0 {
                    errors.push("original execution detected by independent counter".into());
                }
            }
            EventKind::OriginalEntry { .. } => {
                // Global, including unrecognized routes and asynchronous entries
                // arriving after the source window. No in-transaction filter.
                errors.push("original execution detected (including hidden/late work)".into());
            }
            EventKind::GpuCompletion {
                frame,
                submission_id,
                succeeded,
            } => {
                if frames.get(&frame.frame_id).copied() != Some(frame) {
                    errors.push("completion identity does not match source publication".into());
                }
                if !*succeeded || *submission_id == 0 {
                    errors.push("GPU completion failed/invalid".into());
                }
                if completions.insert(*submission_id, frame.frame_id).is_some() {
                    errors.push("duplicate GPU completion/submission identity".into());
                }
            }
            EventKind::Present {
                frame_id,
                submission_id,
                present_id,
            } => {
                if completions.get(submission_id) != Some(frame_id) {
                    errors.push("presentation precedes completion".into());
                }
                if *present_id == 0 || !present_ids.insert(*present_id) {
                    errors.push("invalid/duplicate presentation identity".into());
                }
                presents.push((*frame_id, *submission_id));
            }
        }
    }
    for key in &scope.obligations {
        if !outcomes.contains_key(key) {
            errors.push(format!("missing outcome: {key:?}"));
        }
    }
    for observer in &scope.required_observers {
        if !observed.contains(observer) {
            errors.push(format!("missing observer: {observer:?}"));
        }
    }
    let mut used_submissions = BTreeSet::new();
    let mut rendered_frames = BTreeSet::new();
    for (key, outcome) in outcomes {
        if let Outcome::Replacement { submission_id } = outcome {
            used_submissions.insert(*submission_id);
            if completions.get(submission_id) != Some(&key.frame_id) {
                errors.push("replacement lacks matching successful GPU completion".into());
            } else {
                rendered_frames.insert(key.frame_id);
            }
        }
    }
    for frame in &scope.frames {
        if !rendered_frames.contains(&frame.frame_id) {
            errors.push("frame has no replacement work".into());
        }
    }
    for submission in completions.keys() {
        if !used_submissions.contains(submission) {
            errors.push("completion without replacement output".into());
        }
    }
    let mut presented_frames = BTreeSet::new();
    let mut repeated_presentations = 0;
    for (frame, submission) in &presents {
        if !rendered_frames.contains(frame) || completions.get(submission) != Some(frame) {
            errors.push("presentation lacks completed source-qualified frame".into());
        }
        if !presented_frames.insert(*frame) {
            repeated_presentations += 1;
        }
    }
    let passed = errors.is_empty();
    VerificationReport {
        passed,
        errors,
        unique_completed_frames: if passed { rendered_frames.len() } else { 0 },
        presentations: presents.len(),
        repeated_presentations,
    }
}

fn validate_work(
    key: &WorkKey,
    families: &BTreeSet<&String>,
    frames: &BTreeMap<FrameId, &FrameIdentity>,
    errors: &mut Vec<String>,
) {
    if !families.contains(&key.family) {
        errors.push(format!("unknown family: {}", key.family));
    }
    if !frames.contains_key(&key.frame_id)
        || key.pass_occurrence == 0
        || key.source_item == 0
        || key.stage.trim().is_empty()
    {
        errors.push("invalid work identity/stage".into());
    }
}
