//! Independent stencil history; neither discard nor a store implies coverage.
use super::*;
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub enum StencilLoad {
    Clear(u8),
    Load,
    Discard,
}
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub struct StencilOps {
    pub image: Image,
    pub load: StencilLoad,
    pub store: Store,
    pub source_full_write: bool,
}
fn prior(f: mag_render_abi::packet::StencilFace) -> bool {
    (f.read_mask & 255 != 0 && f.compare != 0 && f.compare != 7)
        || (f.write_mask & 255 != 0 && (f.fail >= 3 || f.depth_fail >= 3 || f.pass >= 3))
}
fn full(f: mag_render_abi::packet::StencilFace) -> bool {
    f.write_mask & 255 == 255
        && f.read_mask & 255 == 0
        && (f.compare == 0 || f.compare == 7)
        && [f.fail, f.depth_fail, f.pass]
            .iter()
            .all(|x| *x == 1 || *x == 2)
}
pub(super) fn validate(
    builder: &SceneBuilder,
    catalog: &impl SceneCatalog,
) -> Result<(), SceneError> {
    let mut initialized = vec![false; builder.images.len()];
    for (index, pass) in builder.passes.iter().enumerate() {
        let stencil = builder.stencils[index];
        if let Some(s) = stencil {
            let slot = initialized
                .get_mut(s.image.index)
                .ok_or(SceneError::WrongScene)?;
            match s.load {
                StencilLoad::Clear(_) => *slot = true,
                StencilLoad::Discard => *slot = false,
                StencilLoad::Load => {
                    if !*slot {
                        return Err(SceneError::Uninitialized);
                    }
                }
            }
            if s.source_full_write {
                let v = builder.viewports[index].ok_or(SceneError::InvalidPass)?;
                let desc = builder.images[s.image.index].plan.desc;
                if pass.draws.len() != 1
                    || v.x != 0.
                    || v.y != 0.
                    || v.width != desc.width as f32
                    || v.height != desc.height as f32
                    || v.scissor_x != 0
                    || v.scissor_y != 0
                    || v.scissor_width != desc.width
                    || v.scissor_height != desc.height
                {
                    return Err(SceneError::InvalidPass);
                }
            }
        }
        for draw in &pass.draws {
            let schema = catalog
                .scene_graphics(draw.pipeline)
                .ok_or(SceneError::UnknownPipeline)?;
            if pass.depth.is_some_and(|d| matches!(d.load, Load::Discard))
                && !matches!(
                    schema.depth.compare,
                    DepthCompare::Disabled | DepthCompare::Never | DepthCompare::Always
                )
            {
                return Err(SceneError::Uninitialized);
            }
            let state = catalog.scene_raster(draw.pipeline);
            if let Some(r) = state.filter(|r| r.stencil_enabled != 0) {
                let s = stencil.ok_or(SceneError::InvalidPass)?;
                let slot = &mut initialized[s.image.index];
                let faces = [
                    (r.cull_mode != 1, r.front_stencil),
                    (r.cull_mode != 2, r.back_stencil),
                ];
                for (active, face) in faces {
                    if active && (!*slot && prior(face) || s.source_full_write && !full(face)) {
                        return Err(SceneError::Uninitialized);
                    }
                }
                if s.source_full_write {
                    *slot = true;
                }
            } else if stencil.is_some_and(|s| s.source_full_write) {
                return Err(SceneError::InvalidPass);
            }
        }
        if let Some(s) = stencil
            && s.store == Store::Discard
        {
            initialized[s.image.index] = false;
        }
    }
    Ok(())
}
