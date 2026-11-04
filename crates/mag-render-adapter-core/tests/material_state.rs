use mag_render_adapter_core::material_state::{
    AlphaComparison, AlphaState, BlendFactor, BlendFactors, PrimaryControlledState,
    PrimaryStateError, PrimaryStateInput, resolve_primary_state,
};
fn prior() -> PrimaryControlledState {
    PrimaryControlledState {
        alpha: AlphaState {
            enabled: true,
            comparison: AlphaComparison::Always,
            reference: 71,
        },
        cull_enabled: false,
        blend_enabled: false,
        blend_factors: BlendFactors {
            source_rgb: BlendFactor::One,
            source_alpha: BlendFactor::Zero,
            destination_rgb: BlendFactor::DestinationColor,
            destination_alpha: BlendFactor::DestinationAlpha,
        },
    }
}
#[test]
fn setting_alpha_comparison_preserves_enable_and_unmentioned_blend_state() {
    let mut input = prior();
    input.alpha.enabled = false;
    let result = resolve_primary_state(
        input,
        PrimaryStateInput {
            flags: 0,
            alpha: 83,
        },
        1,
    )
    .unwrap();
    assert_eq!(
        result.alpha,
        AlphaState {
            enabled: false,
            comparison: AlphaComparison::GreaterOrEqual,
            reference: 83
        }
    );
    assert!(result.cull_enabled);
    assert_eq!(result.blend_enabled, input.blend_enabled);
    assert_eq!(result.blend_factors, input.blend_factors);
    assert_eq!(input.alpha.comparison, AlphaComparison::Always);
}
#[test]
fn disabling_alpha_keeps_inherited_comparison_and_reference() {
    for (alpha, mode) in [(0, 1), (93, 2), (0, 5)] {
        let input = prior();
        let result =
            resolve_primary_state(input, PrimaryStateInput { flags: 1, alpha }, mode).unwrap();
        assert!(!result.alpha.enabled);
        assert_eq!(result.alpha.comparison, input.alpha.comparison);
        assert_eq!(result.alpha.reference, input.alpha.reference);
        assert!(!result.cull_enabled);
    }
}
#[test]
fn mode_five_uses_less_than_full_reference_without_enabling_alpha() {
    let mut input = prior();
    input.alpha.enabled = false;
    let result = resolve_primary_state(
        input,
        PrimaryStateInput {
            flags: 0,
            alpha: 93,
        },
        5,
    )
    .unwrap();
    assert_eq!(
        result.alpha,
        AlphaState {
            enabled: false,
            comparison: AlphaComparison::Less,
            reference: 255
        }
    );
}
#[test]
fn no_write_modes_preserve_entire_prior_primary_state() {
    for (flags, mode) in [(0, 14), (2, 0), (3, 4)] {
        assert_eq!(
            resolve_primary_state(prior(), PrimaryStateInput { flags, alpha: 0 }, mode).unwrap(),
            prior()
        );
    }
}
#[test]
fn mode_four_updates_rgb_and_alpha_factors_without_inventing_blend_enable() {
    for (flags, destination) in [
        (0, BlendFactor::OneMinusSourceAlpha),
        (0x800, BlendFactor::One),
    ] {
        let result =
            resolve_primary_state(prior(), PrimaryStateInput { flags, alpha: 9 }, 4).unwrap();
        assert!(!result.blend_enabled);
        assert_eq!(
            result.blend_factors,
            BlendFactors {
                source_rgb: BlendFactor::SourceAlpha,
                source_alpha: BlendFactor::SourceAlpha,
                destination_rgb: destination,
                destination_alpha: destination
            }
        );
    }
}
#[test]
fn invalid_mode_and_incomplete_material_are_explicit_errors() {
    assert_eq!(
        resolve_primary_state(prior(), PrimaryStateInput { flags: 0, alpha: 0 }, 16),
        Err(PrimaryStateError::InvalidRenderMode)
    );
    assert_eq!(
        PrimaryStateInput::from_material(&[0; 0x5c]),
        Err(PrimaryStateError::MaterialBounds)
    );
    let mut material = [0; 0x60];
    material[0x58..0x5c].copy_from_slice(&0x12345678u32.to_be_bytes());
    material[0x5c] = 191;
    assert_eq!(
        PrimaryStateInput::from_material(&material).unwrap(),
        PrimaryStateInput {
            flags: 0x12345678,
            alpha: 191
        }
    );
}

#[test]
fn inherited_alpha_reference_preserves_the_full_existing_word() {
    let mut input = prior();
    input.alpha.reference = 0xdead_beef;
    let result = resolve_primary_state(input, PrimaryStateInput { flags: 0, alpha: 0 }, 1).unwrap();
    assert_eq!(result.alpha.reference, 0xdead_beef);
    assert!(!result.alpha.enabled);
}
