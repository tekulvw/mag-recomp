//! Authored neutral identities. No guest addresses, layouts or game content.
use serde::{Deserialize, Serialize};

#[derive(Clone, Copy, Debug, PartialEq, Eq, PartialOrd, Ord, Hash, Serialize, Deserialize)]
#[serde(transparent)]
pub struct FrameId(pub u64);
#[derive(Clone, Copy, Debug, PartialEq, Eq, PartialOrd, Ord, Hash, Serialize, Deserialize)]
#[serde(transparent)]
pub struct WorldUpdateId(pub u64);
#[derive(Clone, Copy, Debug, PartialEq, Eq, PartialOrd, Ord, Hash, Serialize, Deserialize)]
#[serde(deny_unknown_fields)]
pub struct ResourceId {
    pub slot: u32,
    pub generation: u32,
}
#[derive(Clone, Copy, Debug, PartialEq, Eq, Serialize, Deserialize)]
#[serde(deny_unknown_fields)]
pub struct ResourceVersion {
    pub id: ResourceId,
    pub content_version: u64,
}
#[derive(Clone, Debug, PartialEq, Eq, Serialize, Deserialize)]
#[serde(deny_unknown_fields)]
pub struct FrameIdentity {
    pub frame_id: FrameId,
    pub world_update_id: WorldUpdateId,
    pub source_publication: u64,
    pub camera_publication: u64,
}
#[derive(Clone, Debug, PartialEq, Eq, PartialOrd, Ord, Hash, Serialize, Deserialize)]
#[serde(deny_unknown_fields)]
pub struct WorkKey {
    pub frame_id: FrameId,
    pub pass_occurrence: u64,
    pub source_item: u64,
    pub family: String,
    pub stage: String,
}
/// An independently armed source-entry route, aggregated for one owned thread.
#[derive(Clone, Debug, PartialEq, Eq, PartialOrd, Ord, Hash, Serialize, Deserialize)]
#[serde(deny_unknown_fields)]
pub struct ObserverKey {
    pub family: String,
    pub stage: String,
    pub route: Route,
    pub thread_id: u64,
}
#[derive(Clone, Copy, Debug, PartialEq, Eq, PartialOrd, Ord, Hash, Serialize, Deserialize)]
#[serde(rename_all = "snake_case")]
pub enum Route {
    PpuEntry,
    SdkLifted,
    SdkInterpreter,
    OutsideTransaction,
}
#[derive(Clone, Debug, Serialize, Deserialize)]
#[serde(deny_unknown_fields)]
pub struct VisibilityEvidence {
    pub evidence_id: String,
    pub key: WorkKey,
}
/// Trusted *input* from upstream inventory, fixed before interception. Never
/// construct this from outcomes. Authentication/source-map completeness belongs
/// to the private collector; this verifier cannot authenticate arbitrary JSON.
#[derive(Clone, Debug, Serialize, Deserialize)]
#[serde(deny_unknown_fields)]
pub struct ScopeManifest {
    pub schema_version: u32,
    pub scope_id: String,
    pub upstream_receipt: String,
    pub window_start: u64,
    pub window_end: u64,
    pub drained_through: u64,
    pub families: Vec<String>,
    pub frames: Vec<FrameIdentity>,
    pub obligations: Vec<WorkKey>,
    pub required_observers: Vec<ObserverKey>,
    pub visibility_evidence: Vec<VisibilityEvidence>,
}
#[derive(Clone, Debug, Serialize, Deserialize)]
#[serde(tag = "kind", rename_all = "snake_case", deny_unknown_fields)]
pub enum Outcome {
    Replacement { submission_id: u64 },
    Culled { evidence_id: String },
    OriginalFallback,
    Failure,
}
#[derive(Clone, Debug, Serialize, Deserialize)]
#[serde(tag = "kind", rename_all = "snake_case", deny_unknown_fields)]
pub enum EventKind {
    Outcome {
        key: WorkKey,
        outcome: Outcome,
    },
    /// A final cumulative counter, including jobs entered after window_end.
    ObserverReceipt {
        observer: ObserverKey,
        covered_from: u64,
        drained_through: u64,
        original_entries: u64,
        lost_records: u64,
        overflow: bool,
        enabled: bool,
    },
    OriginalEntry {
        observer: ObserverKey,
    },
    GpuCompletion {
        frame: FrameIdentity,
        submission_id: u64,
        succeeded: bool,
    },
    Present {
        frame_id: FrameId,
        submission_id: u64,
        present_id: u64,
    },
}
#[derive(Clone, Debug, Serialize, Deserialize)]
#[serde(deny_unknown_fields)]
pub struct Event {
    pub scope_id: String,
    pub sequence: u64,
    pub event: EventKind,
}
