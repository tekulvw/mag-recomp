use std::collections::BTreeMap;

use crate::input::{Context, Control, Source};

#[derive(Clone, Copy, Debug, Eq, PartialEq)]
pub enum Behavior {
    Hold,
    Toggle,
}

#[derive(Clone, Copy, Debug, Eq, PartialEq)]
pub struct Binding {
    pub control: Control,
    pub action: u32,
    pub contexts: u32,
    pub behavior: Behavior,
}

pub(crate) fn valid(bindings: &[Binding]) -> bool {
    if bindings.len() > 1024 {
        return false;
    }
    let mut counts = BTreeMap::new();
    for (index, binding) in bindings.iter().enumerate() {
        if binding.action == 0 || binding.contexts == 0 || binding.contexts & !127 != 0 {
            return false;
        }
        for context in [
            Context::OnFoot,
            Context::Vehicle,
            Context::Turret,
            Context::Tactical,
            Context::Menu,
            Context::Settings,
            Context::Rebinding,
        ] {
            if binding.contexts & context.bit() != 0 {
                let count = counts
                    .entry((
                        binding.action,
                        binding.control.source == Source::Controller,
                        context.bit(),
                    ))
                    .or_insert(0);
                *count += 1;
                if *count > 2 {
                    return false;
                }
            }
        }
        if bindings[..index]
            .iter()
            .any(|other| other.control == binding.control && other.contexts & binding.contexts != 0)
        {
            return false;
        }
    }
    true
}
