use mag_render::types::*;
// Expectations are literal upstream obligations, never inferred from outcomes.
pub fn manifest() -> ScopeManifest {
    serde_json::from_str(r#"{
        "schema_version":1,"scope_id":"synthetic-window","upstream_receipt":"synthetic-upstream-inventory-v1",
        "window_start":1,"window_end":10,"drained_through":20,
        "families":["rigid"],
        "frames":[{"frame_id":1,"world_update_id":9,"source_publication":100,"camera_publication":40}],
        "obligations":[
          {"frame_id":1,"pass_occurrence":1,"source_item":11,"family":"rigid","stage":"prepare"},
          {"frame_id":1,"pass_occurrence":2,"source_item":11,"family":"rigid","stage":"prepare"}],
        "required_observers":[
          {"family":"rigid","stage":"prepare","route":"ppu_entry","thread_id":1},
          {"family":"rigid","stage":"prepare","route":"sdk_lifted","thread_id":1},
          {"family":"rigid","stage":"prepare","route":"sdk_interpreter","thread_id":1},
          {"family":"rigid","stage":"prepare","route":"outside_transaction","thread_id":1}],
        "visibility_evidence":[]
    }"#).unwrap()
}
pub fn events() -> Vec<Event> {
    // Hand-authored producer, observer and terminal receipts are distinct.
    let rows = [
        r#"{"kind":"outcome","key":{"frame_id":1,"pass_occurrence":1,"source_item":11,"family":"rigid","stage":"prepare"},"outcome":{"kind":"replacement","submission_id":7}}"#,
        r#"{"kind":"outcome","key":{"frame_id":1,"pass_occurrence":2,"source_item":11,"family":"rigid","stage":"prepare"},"outcome":{"kind":"replacement","submission_id":7}}"#,
        r#"{"kind":"gpu_completion","frame":{"frame_id":1,"world_update_id":9,"source_publication":100,"camera_publication":40},"submission_id":7,"succeeded":true}"#,
        r#"{"kind":"observer_receipt","observer":{"family":"rigid","stage":"prepare","route":"ppu_entry","thread_id":1},"covered_from":1,"drained_through":20,"original_entries":0,"lost_records":0,"overflow":false,"enabled":true}"#,
        r#"{"kind":"observer_receipt","observer":{"family":"rigid","stage":"prepare","route":"sdk_lifted","thread_id":1},"covered_from":1,"drained_through":20,"original_entries":0,"lost_records":0,"overflow":false,"enabled":true}"#,
        r#"{"kind":"observer_receipt","observer":{"family":"rigid","stage":"prepare","route":"sdk_interpreter","thread_id":1},"covered_from":1,"drained_through":20,"original_entries":0,"lost_records":0,"overflow":false,"enabled":true}"#,
        r#"{"kind":"observer_receipt","observer":{"family":"rigid","stage":"prepare","route":"outside_transaction","thread_id":1},"covered_from":1,"drained_through":20,"original_entries":0,"lost_records":0,"overflow":false,"enabled":true}"#,
        r#"{"kind":"present","frame_id":1,"submission_id":7,"present_id":3}"#,
    ];
    rows.iter()
        .enumerate()
        .map(|(i, row)| {
            let event: EventKind = serde_json::from_str(row).unwrap();
            Event {
                scope_id: "synthetic-window".into(),
                sequence: i as u64 + 1,
                event,
            }
        })
        .collect()
}
