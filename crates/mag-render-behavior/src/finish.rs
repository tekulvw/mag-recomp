//! Main producer: end-of-Main finish (`main-finish.md`).

use crate::effects::{EffectsError, SurfaceBinding, consistent_binding, generation_enabled};
use crate::producers::RendererObject;
use crate::scene_plan::{PlanStep, SetupReferences, transition_steps};
use crate::setup::PipelineState;
use crate::surfaces::{
    SelectedSurface, SourceViewport, ViewportConstants, select, source_viewport,
};
use crate::texture::TextureUnitRecord;
use crate::transitions::{TransitionSources, transition};

/// Carried planner state the finish reads and changes.
#[derive(Clone, Debug, PartialEq)]
pub struct CarriedState {
    pub pipeline: PipelineState,
    pub units: [TextureUnitRecord; 16],
    pub current: SelectedSurface,
    pub viewport: Option<SourceViewport>,
}

/// Setup sources the finish needs.
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub struct FinishSources {
    pub transitions: TransitionSources,
    pub restore: u32,
}

#[derive(Clone, Debug, PartialEq, Eq)]
pub enum FinishError {
    PreviousGroup(i32),
    MissingSources,
    Transition(crate::transitions::TransitionError),
    Producer(EffectsError),
    RestoreReference(u32),
    GenerationFlag,
    Binding(EffectsError),
    UnknownUnit(u32),
}

/// Runs the finish. `run_step` performs producer events and water rows raised by the remaining
/// transitions on the carried state; the pipeline transitions themselves are applied here, in
/// the walk's order. Returns the steps performed.
#[allow(clippy::too_many_arguments)]
pub fn finish(
    previous_group: i32,
    sources: Option<&FinishSources>,
    generation: Option<u32>,
    renderer: &RendererObject,
    bindings: &[SurfaceBinding],
    viewport_constants: &ViewportConstants,
    state: &mut CarriedState,
    run_step: &mut dyn FnMut(PlanStep, &mut CarriedState) -> Result<(), EffectsError>,
) -> Result<Vec<PlanStep>, FinishError> {
    if !(-1..=15).contains(&previous_group) {
        return Err(FinishError::PreviousGroup(previous_group));
    }
    let sources = sources.ok_or(FinishError::MissingSources)?;
    let t = &sources.transitions;
    let references = SetupReferences {
        lists_enabled: t.lists_enabled,
        initial: t.initial,
        group_thirteen: t.group_thirteen,
        group_fifteen: t.group_fifteen,
    };
    let cleared = TransitionSources {
        initial: [0; 3],
        group_thirteen: 0,
        group_fifteen: [0; 2],
        ..*t
    };
    let steps = if previous_group == 15 {
        Vec::new()
    } else {
        transition_steps(previous_group, 15, &references)
    };
    for step in &steps {
        match *step {
            PlanStep::Transition { from, to } => {
                transition(&mut state.pipeline, from, to as i32, &cleared)
                    .map_err(FinishError::Transition)?;
            }
            other => run_step(other, state).map_err(FinishError::Producer)?,
        }
    }
    if !t.lists_enabled {
        return Ok(steps);
    }
    if sources.restore == 0 || sources.restore % 4 != 0 {
        return Err(FinishError::RestoreReference(sources.restore));
    }
    let mut next = state.clone();
    match generation {
        None => return Err(FinishError::GenerationFlag),
        Some(flag) if generation_enabled(flag) => {
            let surface = select(renderer, state.current.index, 2)
                .map_err(|_| FinishError::Binding(EffectsError::Association))?;
            consistent_binding(bindings, &surface).map_err(FinishError::Binding)?;
            next.viewport = Some(
                source_viewport(&surface, viewport_constants)
                    .map_err(|_| FinishError::Binding(EffectsError::Viewport))?,
            );
            next.current = surface;
        }
        Some(_) => {}
    }
    let p = &mut next.pipeline;
    p.polygon_offset_fill = Some(false);
    p.srgb_output = Some(false);
    p.colour_masks = Some([15; 4]);
    p.depth_write = Some(true);
    p.blend_enable = Some(false);
    for unit in [11, 15] {
        if next.units[unit].compact.is_none() {
            return Err(FinishError::UnknownUnit(unit as u32));
        }
        next.units[unit].disable_if_known();
    }
    *state = next;
    Ok(steps)
}

#[cfg(test)]
mod tests {
    use super::*;
    use crate::effects::tests::{all_bindings, renderer};
    use crate::scene_plan::ProducerEvent;

    const SEEDED: [u32; 8] = [0x55, 0x66, 0x77, 0xffff_ffff, 0x99, 0xaa, 0xbb, 0xcc];

    fn carried(r: &RendererObject) -> CarriedState {
        let mut units: [TextureUnitRecord; 16] = Default::default();
        units[11].compact = Some(SEEDED);
        units[15].compact = Some(SEEDED);
        CarriedState {
            pipeline: PipelineState {
                depth_bounds_enable: Some(true),
                depth_test: Some(false),
                ..Default::default()
            },
            units,
            current: select(r, 0, 1).unwrap(),
            viewport: None,
        }
    }

    fn sources(lists: bool, restore: u32) -> FinishSources {
        FinishSources {
            transitions: TransitionSources {
                lists_enabled: lists,
                ..Default::default()
            },
            restore,
        }
    }

    fn vc() -> ViewportConstants {
        ViewportConstants {
            vp_hx: 0.5,
            vp_hy: -0.5,
            clip_min: 0.0,
            clip_max: 1.0,
            jitter: [0.0, 0.0],
        }
    }

    fn run(
        previous: i32,
        s: Option<&FinishSources>,
        generation: Option<u32>,
        state: &mut CarriedState,
    ) -> Result<Vec<PlanStep>, FinishError> {
        let r = renderer(true);
        let bindings = all_bindings(&r);
        finish(
            previous,
            s,
            generation,
            &r,
            &bindings,
            &vc(),
            state,
            &mut |_, _| Ok(()),
        )
    }

    #[test]
    fn v1_from_group_fourteen() {
        let r = renderer(true);
        let mut state = carried(&r);
        run(14, Some(&sources(true, 0x41c)), Some(1), &mut state).unwrap();
        assert_eq!(state.current.address, r.address + 0x2c10 + 0x150);
        let p = state.pipeline;
        assert_eq!(
            (
                p.depth_write,
                p.colour_masks,
                p.blend_enable,
                p.srgb_output,
                p.polygon_offset_fill
            ),
            (
                Some(true),
                Some([15; 4]),
                Some(false),
                Some(false),
                Some(false)
            )
        );
        assert_eq!(p.depth_bounds_enable, Some(true));
        assert_eq!(
            p.depth_test,
            Some(true),
            "the group-15 transition sets depth test on"
        );
        assert_eq!(
            state.units[11].compact,
            Some([0x55, 0x66, 0x77, 0, 0x99, 0xaa, 0xbb, 0xcc])
        );
        assert_eq!(
            state.units[15].compact,
            Some([0x55, 0x66, 0x77, 0, 0x99, 0xaa, 0xbb, 0xcc])
        );
        assert!(state.viewport.is_some());
    }

    #[test]
    fn v2_from_group_fifteen() {
        let r = renderer(true);
        let mut state = carried(&r);
        let steps = run(15, Some(&sources(true, 0x41c)), Some(1), &mut state).unwrap();
        assert!(steps.is_empty());
        assert_eq!(state.pipeline.depth_test, Some(false));
    }

    #[test]
    fn v3_v4_errors() {
        let r = renderer(true);
        let mut state = carried(&r);
        assert_eq!(
            run(16, Some(&sources(true, 0x41c)), Some(1), &mut state),
            Err(FinishError::PreviousGroup(16))
        );
        assert_eq!(
            run(-2, Some(&sources(true, 0x41c)), Some(1), &mut state),
            Err(FinishError::PreviousGroup(-2))
        );
        assert_eq!(
            run(15, Some(&sources(true, 0x41e)), Some(1), &mut state),
            Err(FinishError::RestoreReference(0x41e))
        );
        assert_eq!(
            run(15, Some(&sources(true, 0)), Some(1), &mut state),
            Err(FinishError::RestoreReference(0))
        );
        assert_eq!(
            run(15, None, Some(1), &mut state),
            Err(FinishError::MissingSources)
        );
        assert_eq!(
            run(15, Some(&sources(true, 0x41c)), None, &mut state),
            Err(FinishError::GenerationFlag)
        );
    }

    #[test]
    fn v5_lists_disabled() {
        let r = renderer(true);
        let mut state = carried(&r);
        run(15, Some(&sources(false, 0)), Some(1), &mut state).unwrap();
        assert_eq!(state.units[11].compact.unwrap()[3], 0xffff_ffff);
    }

    #[test]
    fn v6_guard_disabled() {
        let r = renderer(true);
        let mut state = carried(&r);
        run(15, Some(&sources(true, 0x41c)), Some(0x100), &mut state).unwrap();
        assert_eq!(state.current.address, r.address + 0x2c10 + 0xa8);
        assert_eq!(state.pipeline.depth_write, Some(true));
    }

    #[test]
    fn v7_unknown_unit() {
        let r = renderer(true);
        let mut state = carried(&r);
        state.units[15].compact = None;
        let before = state.clone();
        assert_eq!(
            run(15, Some(&sources(true, 0x41c)), Some(1), &mut state),
            Err(FinishError::UnknownUnit(15))
        );
        assert_eq!(state, before);
    }

    #[test]
    fn producer_events_run_in_order() {
        let r = renderer(true);
        let bindings = all_bindings(&r);
        let mut state = carried(&r);
        let s = FinishSources {
            transitions: TransitionSources {
                lists_enabled: true,
                group_thirteen: 0x300,
                group_fifteen: [0x400, 0],
                ..Default::default()
            },
            restore: 0x41c,
        };
        let mut seen = Vec::new();
        finish(
            11,
            Some(&s),
            Some(1),
            &r,
            &bindings,
            &vc(),
            &mut state,
            &mut |step, _| {
                seen.push(step);
                Ok(())
            },
        )
        .unwrap();
        assert_eq!(
            seen,
            vec![
                PlanStep::Event(ProducerEvent::Group12Textures),
                PlanStep::WaterRows,
                PlanStep::Event(ProducerEvent::Group13 { target: 0x300 }),
                PlanStep::Event(ProducerEvent::Group15 {
                    slot: 0,
                    target: 0x400
                }),
            ]
        );
        let mut failing = carried(&r);
        assert_eq!(
            finish(
                11,
                Some(&s),
                Some(1),
                &r,
                &bindings,
                &vc(),
                &mut failing,
                &mut |_, _| Err(EffectsError::Alias)
            ),
            Err(FinishError::Producer(EffectsError::Alias))
        );
    }
}
