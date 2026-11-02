//! Neutral synthetic receipt verifier; no backend or private source reader.
use mag_render::{
    accounting::{VerificationReport, verify_removal},
    types::{Event, ScopeManifest},
};
use std::{
    collections::BTreeMap,
    ffi::OsString,
    fs::{self, File},
    io::Read,
    path::{Path, PathBuf},
};
const MAX_INPUT_BYTES: u64 = 64 * 1024 * 1024;

fn arguments() -> Result<BTreeMap<String, PathBuf>, String> {
    let mut args = std::env::args_os().skip(1);
    let mut paths = BTreeMap::new();
    while let Some(key) = args.next() {
        let key = key.into_string().map_err(|_| "invalid argument encoding")?;
        if !["--scope", "--events", "--report"].contains(&key.as_str()) || paths.contains_key(&key)
        {
            return Err(format!("unknown/duplicate option: {key}"));
        }
        let value = args.next().ok_or_else(|| format!("missing path: {key}"))?;
        if value == OsString::new() {
            return Err("empty path".into());
        }
        paths.insert(key, PathBuf::from(value));
    }
    if paths.len() != 3 {
        return Err("usage: render_verify --scope <json> --events <jsonl> --report <json>".into());
    }
    let report = identity(&paths["--report"])?;
    if report == identity(&paths["--scope"])? || report == identity(&paths["--events"])? {
        return Err("report must differ from input files".into());
    }
    Ok(paths)
}
fn identity(path: &Path) -> Result<PathBuf, String> {
    if path.exists() {
        path.canonicalize().map_err(|e| format!("path: {e}"))
    } else {
        let parent = path
            .parent()
            .filter(|p| !p.as_os_str().is_empty())
            .unwrap_or(Path::new("."));
        let name = path.file_name().ok_or("invalid path")?;
        Ok(parent
            .canonicalize()
            .map_err(|e| format!("path parent: {e}"))?
            .join(name))
    }
}
fn bounded_read(path: &Path) -> Result<String, String> {
    let file = File::open(path).map_err(|e| format!("open {}: {e}", path.display()))?;
    let mut bytes = Vec::new();
    file.take(MAX_INPUT_BYTES + 1)
        .read_to_end(&mut bytes)
        .map_err(|e| format!("read: {e}"))?;
    if bytes.len() as u64 > MAX_INPUT_BYTES {
        return Err("input exceeds 64 MiB bound".into());
    }
    String::from_utf8(bytes).map_err(|e| format!("input UTF-8: {e}"))
}
fn verify(paths: &BTreeMap<String, PathBuf>) -> Result<VerificationReport, String> {
    let scope: ScopeManifest = serde_json::from_str(&bounded_read(&paths["--scope"])?)
        .map_err(|e| format!("scope JSON: {e}"))?;
    let input = bounded_read(&paths["--events"])?;
    let events: Vec<Event> = input
        .lines()
        .enumerate()
        .map(|(i, line)| {
            serde_json::from_str(line).map_err(|e| format!("event line {}: {e}", i + 1))
        })
        .collect::<Result<_, _>>()?;
    Ok(verify_removal(&scope, &events))
}
fn run() -> i32 {
    let paths = match arguments() {
        Ok(v) => v,
        Err(e) => {
            eprintln!("{e}");
            return 2;
        }
    };
    let report = match verify(&paths) {
        Ok(r) => r,
        Err(e) => VerificationReport {
            passed: false,
            errors: vec![e],
            unique_completed_frames: 0,
            presentations: 0,
            repeated_presentations: 0,
        },
    };
    let encoded = match serde_json::to_vec_pretty(&report) {
        Ok(v) => v,
        Err(e) => {
            eprintln!("report JSON: {e}");
            return 2;
        }
    };
    if let Err(e) = fs::write(&paths["--report"], encoded) {
        eprintln!("write report: {e}");
        return 2;
    }
    if report.passed { 0 } else { 1 }
}
fn main() {
    std::process::exit(run());
}
