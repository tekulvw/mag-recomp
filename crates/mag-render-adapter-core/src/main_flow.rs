//! Main per-item source routes, before material/geometry preparation.
//! This is not final draw admission or duplicate/group-transition state.
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub enum MainFlowError {
    Group(u16),
}
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub struct MainCaller {
    pub group_zero: bool,
    pub binding_control: bool,
    pub depth_axis_projection: bool,
    /// Force both material and technique binds independently of prior keys.
    pub force_material_and_technique: bool,
}
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub enum MainItemRoute {
    /// Prior processed-item state still commits; no ordinary item preparation.
    UpdateOnly,
    Prepare(MainCaller),
    /// Stateful special caller; not equivalent to an ordinary item emitter.
    Special14,
    Special15,
}
/// Route one Main item by its material group and the pass control word (the
/// unsigned word at pass +0x144). Group 0 only updates the processed-item state
/// unless the pass control exceeds 2. Every source group stays visible.
/// Duplicate suppression and group state transitions are decided before this
/// route and remain separate inputs.
pub fn main_item_route(group: u16, pass_control: u32) -> Result<MainItemRoute, MainFlowError> {
    match group {
        0 if pass_control <= 2 => Ok(MainItemRoute::UpdateOnly),
        0..=13 => Ok(MainItemRoute::Prepare(MainCaller {
            group_zero: group == 0,
            binding_control: match group {
                0 | 10 => true,
                8 | 9 | 12 | 13 => false,
                _ => pass_control != 5,
            },
            depth_axis_projection: group == 11,
            force_material_and_technique: group == 13,
        })),
        14 => Ok(MainItemRoute::Special14),
        15 => Ok(MainItemRoute::Special15),
        other => Err(MainFlowError::Group(other)),
    }
}
