//! Owned cohort records as planner inputs, and agreement with the adapter core's routes.

use mag_render_adapter_core::cohort::{CohortHeader as CoreHeader, CohortItem};
use mag_render_behavior::scene_plan::{CohortHeader, DrawSources, TraversalItem};

/// The planner's cohort header from the adapter core's.
pub fn cohort_header(header: &CoreHeader) -> CohortHeader {
    CohortHeader {
        source_invocation: header.source_invocation,
        execution_update: header.execution_update,
        pass_occurrence: header.pass_occurrence,
    }
}

/// One material entry as the planner sees it: `None` is a refusal, `Some(None)` has no draw
/// sources.
pub type MaterialEntry = Option<Option<DrawSources>>;

/// Traversal items from cohort items and their material entries, in item order. Returns `None`
/// when the two lists differ in length (an association failure).
pub fn traversal_items(
    items: &[CohortItem],
    materials: &[MaterialEntry],
) -> Option<Vec<TraversalItem>> {
    if items.len() != materials.len() {
        return None;
    }
    Some(
        items
            .iter()
            .zip(materials)
            .map(|(item, material)| TraversalItem {
                mbi: item.mbi,
                transform: item.transform,
                material: *material,
            })
            .collect(),
    )
}

#[cfg(test)]
mod tests {
    use super::*;
    use mag_render_adapter_core::main_flow::{MainItemRoute, main_item_route};
    use mag_render_behavior::material_values::{
        Identity, ItemInputs, PassValues, TechniqueRecord, TransformRecord, binding_row,
    };
    use mag_render_behavior::scene_plan::{Route, route};

    fn item(group: u32, control: u32) -> TraversalItem {
        let mut mbi = [0u8; 32];
        mbi[0] = (group << 4) as u8;
        let transform = [0u8; 112];
        TraversalItem {
            mbi,
            transform: Some(transform),
            material: Some(Some(DrawSources {
                group,
                transform,
                pass_control: control,
            })),
        }
    }

    /// The behaviour planner's route and the adapter core's route agree for every group and a
    /// spread of pass controls.
    #[test]
    fn routes_agree_with_the_adapter_core() {
        for group in 0..16u32 {
            for control in [0, 1, 2, 3, 4, 5, 6, 0x7fff_ffff, 0x8000_0000, 0xffff_ffff] {
                let ours = route(&item(group, control), group, control, None, -1).unwrap();
                let core = main_item_route(group as u16, control).unwrap();
                let same = matches!(
                    (ours, core),
                    (Route::UpdateOnly, MainItemRoute::UpdateOnly)
                        | (Route::Prepare, MainItemRoute::Prepare(_))
                        | (Route::Special14, MainItemRoute::Special14)
                        | (Route::Special15, MainItemRoute::Special15)
                );
                assert!(
                    same,
                    "group {group} control {control:#x}: {ours:?} vs {core:?}"
                );
            }
        }
    }

    /// The adapter core's binding control is the complement of the binding row's lane-2 rule.
    #[test]
    fn binding_control_matches_binding_row_lane_two() {
        for group in 0..14u32 {
            for control in [0u32, 3, 5, 6] {
                let Ok(MainItemRoute::Prepare(caller)) = main_item_route(group as u16, control)
                else {
                    continue;
                };
                let mut inputs = ItemInputs {
                    identity: Identity {
                        material: 1,
                        technique: 1,
                        selector: 0,
                        group,
                    },
                    technique: TechniqueRecord {
                        membership_bit: 0,
                        first_parameter: 0,
                        staging_first: 0,
                        staging_count: 0,
                        flags: 0,
                    },
                    parameters: &[],
                    material_class: 0,
                    material_flags: 0,
                    mbi: [0; 32],
                    transform: None::<TransformRecord>,
                    parameter_column: mag_render_behavior::material_values::ParameterColumn::Unused,
                    mesh_constant: [0; 4],
                    pass: PassValues {
                        depth_anchor: 0,
                        depth_scale: 0,
                        view: [0; 16],
                        lane3: 0,
                        distance_words: [0x3f80_0000; 6],
                        parameter_column_base: 0,
                        control,
                    },
                    static_rows: &[],
                    builtin_rows: &[],
                };
                inputs.pass.control = control;
                let lane2 = binding_row(&inputs).unwrap()[2];
                assert_eq!(
                    caller.binding_control,
                    lane2 == 0,
                    "group {group} control {control}"
                );
            }
        }
    }

    #[test]
    fn items_and_header_convert() {
        let header = CoreHeader {
            vm: 1,
            source_invocation: 2,
            ppu_owner: 3,
            pass_occurrence: 4,
            execution_update: 5,
            camera: 0,
            group_counts: [[0; 16]; 4],
        };
        let converted = cohort_header(&header);
        assert_eq!(
            (
                converted.source_invocation,
                converted.execution_update,
                converted.pass_occurrence
            ),
            (2, 5, 4)
        );
        let items = [CohortItem {
            shard: 0,
            source_index: 0,
            mbi: [0x30; 32],
            transform: None,
        }];
        let traversal = traversal_items(&items, &[None]).unwrap();
        assert_eq!(traversal[0].census_group(), 3);
        assert!(traversal_items(&items, &[]).is_none());
    }
}
