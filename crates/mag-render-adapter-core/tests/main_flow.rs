use mag_render_adapter_core::main_flow::*;
#[test]
fn group_zero_only_commits_prior_state_for_unsigned_pass_controls_zero_through_two() {
    for control in [0, 1, 2] {
        assert_eq!(main_item_route(0, control), Ok(MainItemRoute::UpdateOnly));
    }
    for control in [3, 5, 0x8000_0000, u32::MAX] {
        assert_eq!(
            main_item_route(0, control),
            Ok(MainItemRoute::Prepare(MainCaller {
                group_zero: true,
                binding_control: true,
                depth_axis_projection: false,
                force_material_and_technique: false
            }))
        );
    }
}
#[test]
fn ordinary_binding_control_is_source_group_and_pass_dependent() {
    for group in 1..=13 {
        for control in [0, 2, 3, 5, u32::MAX] {
            let actual = match main_item_route(group, control).unwrap() {
                MainItemRoute::Prepare(c) => c,
                _ => panic!("ordinary source route"),
            };
            assert!(!actual.group_zero);
            assert_eq!(
                actual.binding_control,
                match group {
                    8 | 9 | 12 | 13 => false,
                    10 => true,
                    _ => control != 5,
                }
            );
            assert_eq!(actual.depth_axis_projection, group == 11);
            assert_eq!(actual.force_material_and_technique, group == 13);
        }
    }
}
#[test]
fn special_groups_stay_distinct_without_being_silently_routed_as_ordinary_geometry() {
    for control in [0, 3, 5, u32::MAX] {
        assert_eq!(main_item_route(14, control), Ok(MainItemRoute::Special14));
        assert_eq!(main_item_route(15, control), Ok(MainItemRoute::Special15));
    }
    assert_eq!(main_item_route(16, 3), Err(MainFlowError::Group(16)));
}
