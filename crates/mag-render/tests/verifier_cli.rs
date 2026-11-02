mod common;
use common::{events, manifest};
use mag_render::types::*;
use std::{
    fs,
    process::Command,
    sync::atomic::{AtomicU64, Ordering},
};
static NEXT: AtomicU64 = AtomicU64::new(1);
struct Fixture(std::path::PathBuf);
impl Fixture {
    fn new() -> Self {
        let p = std::env::temp_dir().join(format!(
            "mag render verify {} {}",
            std::process::id(),
            NEXT.fetch_add(1, Ordering::Relaxed)
        ));
        fs::create_dir(&p).unwrap();
        Self(p)
    }
    fn run(&self, s: &ScopeManifest, e: &[Event]) -> (bool, serde_json::Value) {
        fs::write(self.0.join("scope.json"), serde_json::to_vec(s).unwrap()).unwrap();
        fs::write(
            self.0.join("events.jsonl"),
            e.iter()
                .map(|e| serde_json::to_string(e).unwrap() + "\n")
                .collect::<String>(),
        )
        .unwrap();
        let status = Command::new(env!("CARGO_BIN_EXE_render_verify"))
            .args(["--scope"])
            .arg(self.0.join("scope.json"))
            .arg("--events")
            .arg(self.0.join("events.jsonl"))
            .arg("--report")
            .arg(self.0.join("report.json"))
            .status()
            .unwrap();
        let report =
            serde_json::from_slice(&fs::read(self.0.join("report.json")).unwrap()).unwrap();
        (status.success(), report)
    }
}
impl Drop for Fixture {
    fn drop(&mut self) {
        let _ = fs::remove_dir_all(&self.0);
    }
}
#[test]
fn cli_accepts_only_complete_receipts_and_writes_reports() {
    let f = Fixture::new();
    let (ok, r) = f.run(&manifest(), &events());
    assert!(ok);
    assert_eq!(r["passed"], true);
    assert_eq!(r["unique_completed_frames"], 1);
    let mut repeated = events();
    repeated.push(Event {
        scope_id: "synthetic-window".into(),
        sequence: 9,
        event: EventKind::Present {
            frame_id: FrameId(1),
            submission_id: 7,
            present_id: 4,
        },
    });
    let (ok, r) = f.run(&manifest(), &repeated);
    assert!(ok);
    assert_eq!(r["unique_completed_frames"], 1);
    assert_eq!(r["presentations"], 2);
    assert_eq!(r["repeated_presentations"], 1);
    for case in 0..10 {
        let mut s = manifest();
        let mut e = events();
        match case {
            0 => {
                e.remove(0);
            }
            1 => {
                let mut extra = e[0].clone();
                extra.sequence = 9;
                e.push(extra);
            }
            2 => {
                if let EventKind::ObserverReceipt {
                    original_entries, ..
                } = &mut e[4].event
                {
                    *original_entries = 1;
                }
            }
            3 => {
                e.push(Event {
                    scope_id: s.scope_id.clone(),
                    sequence: 19,
                    event: EventKind::OriginalEntry {
                        observer: s.required_observers[0].clone(),
                    },
                });
            }
            4 => {
                if let EventKind::Outcome { key, .. } = &mut e[0].event {
                    key.family = "unknown".into();
                }
            }
            5 => {
                for event in &mut e {
                    if let EventKind::Outcome { outcome, .. } = &mut event.event {
                        *outcome = Outcome::Culled {
                            evidence_id: "invented".into(),
                        };
                    }
                }
            }
            6 => {
                e.remove(4);
            }
            7 => {
                s.obligations.clear();
                s.frames.clear();
            }
            8 => {
                let mut renamed = s.frames[0].clone();
                renamed.frame_id = FrameId(2);
                s.frames.push(renamed);
            }
            _ => {
                if let EventKind::GpuCompletion { succeeded, .. } = &mut e[2].event {
                    *succeeded = false;
                }
            }
        }
        let (ok, r) = f.run(&s, &e);
        assert!(!ok, "negative fixture {case}");
        assert_eq!(r["passed"], false);
        assert!(!r["errors"].as_array().unwrap().is_empty());
    }
}
#[test]
fn cli_malformed_inputs_and_unknown_args_fail_closed() {
    let f = Fixture::new();
    let _ = f.run(&manifest(), &events());
    fs::write(f.0.join("events.jsonl"), "{malformed}\n").unwrap();
    let status = Command::new(env!("CARGO_BIN_EXE_render_verify"))
        .arg("--scope")
        .arg(f.0.join("scope.json"))
        .arg("--events")
        .arg(f.0.join("events.jsonl"))
        .arg("--report")
        .arg(f.0.join("report.json"))
        .status()
        .unwrap();
    assert!(!status.success());
    let r: serde_json::Value =
        serde_json::from_slice(&fs::read(f.0.join("report.json")).unwrap()).unwrap();
    assert_eq!(r["passed"], false);
    assert!(
        !Command::new(env!("CARGO_BIN_EXE_render_verify"))
            .arg("--unknown")
            .status()
            .unwrap()
            .success()
    );
}
