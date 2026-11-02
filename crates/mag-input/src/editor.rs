//! Transactional binding edits shared by native settings frontends.
use crate::bindings::Binding;
use crate::input::Source;

#[derive(Clone, Copy, Debug, Eq, PartialEq)]
pub enum EditError {
    Invalid,
    Conflict(u32),
}

/// Return a new valid binding list. Failure never changes the caller's draft.
pub fn edit(
    original: &[Binding],
    request: Binding,
    slot: u32,
    clear: bool,
    replace: bool,
) -> Result<Vec<Binding>, EditError> {
    if slot > 1
        || request.contexts.count_ones() != 1
        || request.contexts & !127 != 0
        || request.action == 0
        || !crate::bindings::valid(original)
    {
        return Err(EditError::Invalid);
    }
    let family = |b: &Binding| {
        (b.control.source == Source::Controller) == (request.control.source == Source::Controller)
    };
    let target = original
        .iter()
        .enumerate()
        .filter(|(_, b)| {
            b.action == request.action && family(b) && b.contexts & request.contexts != 0
        })
        .nth(slot as usize)
        .map(|(i, _)| i);
    if !clear {
        for (i, b) in original.iter().enumerate() {
            if Some(i) != target
                && b.control == request.control
                && b.contexts & request.contexts != 0
                && !replace
            {
                return Err(EditError::Conflict(b.action));
            }
        }
    }
    let mut result = Vec::new();
    for (i, b) in original.iter().enumerate() {
        let mut retained = *b;
        if Some(i) == target || (!clear && retained.control == request.control) {
            retained.contexts &= !request.contexts;
        }
        if retained.contexts != 0 {
            result.push(retained);
        }
        if Some(i) == target && !clear {
            result.push(request);
        }
    }
    if target.is_none() && !clear {
        result.push(request);
    }
    if !crate::bindings::valid(&result) {
        return Err(EditError::Invalid);
    }
    Ok(result)
}

#[cfg(test)]
mod tests {
    use super::*;
    use crate::bindings::Behavior;
    use crate::input::{Control, Source};
    fn binding(code: u32, action: u32, contexts: u32) -> Binding {
        Binding {
            control: Control {
                source: Source::Keyboard,
                code,
            },
            action,
            contexts,
            behavior: Behavior::Hold,
        }
    }
    #[test]
    fn canceled_conflict_keeps_original_and_replace_preserves_disjoint_context() {
        let original = vec![binding(4, 1, 3), binding(7, 2, 1)];
        let request = binding(4, 2, 1);
        assert_eq!(
            edit(&original, request, 0, false, false),
            Err(EditError::Conflict(1))
        );
        assert_eq!(original[0].contexts, 3);
        let result = edit(&original, request, 0, false, true).unwrap();
        assert_eq!(result, vec![binding(4, 1, 2), binding(4, 2, 1)]);
    }
    #[test]
    fn secondary_clear_and_replace_do_not_touch_other_device_family() {
        let mut original = vec![binding(4, 1, 1), binding(7, 1, 1), binding(8, 2, 2)];
        let mut controller = binding(0, 1, 1);
        controller.control.source = Source::Controller;
        original.push(controller);
        let result = edit(&original, binding(9, 1, 1), 1, false, false).unwrap();
        assert_eq!(result[0], original[0]);
        assert_eq!(result[1].control.code, 9);
        assert_eq!(result[3], controller);
        let cleared = edit(&result, binding(0, 1, 1), 0, true, false).unwrap();
        assert_eq!(cleared.len(), 3);
        assert_eq!(cleared[0].control.code, 9);
    }
    #[test]
    fn disjoint_context_reuse_is_allowed_and_invalid_slot_is_rejected() {
        let original = vec![binding(4, 1, 2)];
        assert!(edit(&original, binding(4, 2, 1), 0, false, false).is_ok());
        assert_eq!(
            edit(&original, binding(5, 1, 1), 2, false, false),
            Err(EditError::Invalid)
        );
    }
}
