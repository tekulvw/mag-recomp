//! Plan steps and producer passes as packet entries.
//!
//! The assembler owns the passes and items; it does not build the ABI's pointer-bearing records
//! (`Desc`, `Pass`, `Draw`). Those are written by the adapter's packet builder from these owned
//! entries, which map one to one onto them.

use mag_render_abi::packet::{
    ITEM_DRAW_REQUIRED, ITEM_SOURCE_LISTS_DISABLED, ITEM_SOURCE_REPEATED, ITEM_SOURCE_UPDATE_ONLY,
    Item, StencilAttachment,
};
use mag_render_behavior::effects::{
    AttachmentImage, EffectsError, LoadOp, ProducerPass, SurfaceBinding, open_main_pass,
};
use mag_render_behavior::scene_plan::{
    Attachment as ClearAttachment, ClearPass, NoDrawReason, PlanStep,
};

/// Packet attachment load codes.
pub const LOAD_CLEAR: u32 = 0;
pub const LOAD_LOAD: u32 = 1;
pub const LOAD_DONT_CARE: u32 = 2;
/// Packet attachment store codes.
pub const STORE_STORE: u32 = 0;
pub const STORE_DISCARD: u32 = 1;
/// Image index of an absent attachment.
pub const NO_IMAGE: u32 = u32::MAX;
/// Producer item identities have the top bit set.
pub const PRODUCER_IDENTITY: u64 = 1 << 63;

/// An owned attachment, field for field `mag_render_abi::Attachment`.
#[derive(Clone, Copy, Debug, PartialEq)]
pub struct OwnedAttachment {
    pub image: u32,
    pub load: u32,
    pub store: u32,
    pub clear: [f32; 4],
}

impl OwnedAttachment {
    pub const ABSENT: Self = Self {
        image: NO_IMAGE,
        load: 0,
        store: 0,
        clear: [0.0; 4],
    };

    fn from_image(image: Option<AttachmentImage>) -> Self {
        match image {
            None => Self::ABSENT,
            Some(a) => Self {
                image: a.image,
                load: match a.load {
                    LoadOp::Clear => LOAD_CLEAR,
                    LoadOp::Load => LOAD_LOAD,
                    LoadOp::DontCare => LOAD_DONT_CARE,
                },
                store: STORE_STORE,
                clear: [0.0; 4],
            },
        }
    }

    pub fn to_abi(self) -> mag_render_abi::Attachment {
        mag_render_abi::Attachment {
            image: self.image,
            load: self.load,
            store: self.store,
            reserved: 0,
            clear: self.clear,
        }
    }
}

/// What a pass is associated with.
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub enum PassSource {
    Main { surface: u32 },
    Producer { target: u32 },
    RetainedClear,
}

/// An owned pass with its draws.
#[derive(Clone, Debug, PartialEq)]
pub struct OwnedPass<D> {
    pub source: PassSource,
    pub colour: OwnedAttachment,
    pub depth: OwnedAttachment,
    pub stencil: Option<StencilAttachment>,
    pub draws: Vec<D>,
}

/// An owned item, field for field `mag_render_abi::packet::Item`.
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub struct OwnedItem {
    pub pass: u32,
    pub kind: u32,
    pub source_item: u64,
}

impl OwnedItem {
    pub fn to_abi(self) -> Item {
        Item {
            pass: self.pass,
            reserved: self.kind,
            source_item: self.source_item,
            unresolved_obligations: 0,
        }
    }
}

/// Work the caller performs for a plan step.
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub enum CallerWork {
    /// Run the producer event, the transition, the water rows or the update-only observation.
    Step(PlanStep),
    /// Prepare the item's draws into the open pass (`add_draw`).
    Prepare { item: usize, group: u32 },
}

/// Assembles passes and items in plan order.
#[derive(Clone, Debug)]
pub struct PacketAssembler<D> {
    pub passes: Vec<OwnedPass<D>>,
    pub items: Vec<OwnedItem>,
    open: bool,
    producer_items: u64,
}

impl<D> Default for PacketAssembler<D> {
    fn default() -> Self {
        Self {
            passes: Vec::new(),
            items: Vec::new(),
            open: false,
            producer_items: 0,
        }
    }
}

fn stencil(image: Option<AttachmentImage>) -> Option<StencilAttachment> {
    image.map(|a| StencilAttachment {
        image: a.image,
        load: OwnedAttachment::from_image(Some(a)).load,
        store: STORE_STORE,
        clear_value: 0,
        flags: 0,
        reserved: 0,
    })
}

impl<D> PacketAssembler<D> {
    pub fn new() -> Self {
        Self::default()
    }

    fn current_pass(&self) -> u32 {
        (self.passes.len() - 1) as u32
    }

    /// Applies one plan step. `main` is the current Main surface binding (producers may have
    /// changed it) and `selector` the colour-target selector. Returns the work the caller must
    /// perform for this step, if any.
    pub fn apply(
        &mut self,
        step: PlanStep,
        main: &SurfaceBinding,
        selector: Option<u32>,
    ) -> Result<Option<CallerWork>, EffectsError> {
        match step {
            PlanStep::OpenPass => {
                open_main_pass(selector, main)?;
                self.passes.push(OwnedPass {
                    source: PassSource::Main {
                        surface: main.address,
                    },
                    colour: OwnedAttachment::from_image(main.colour),
                    depth: OwnedAttachment::from_image(main.depth),
                    stencil: stencil(main.stencil),
                    draws: Vec::new(),
                });
                self.open = true;
                Ok(None)
            }
            PlanStep::NoDraw { item, reason } => {
                let kind = match reason {
                    NoDrawReason::ListsDisabled => ITEM_SOURCE_LISTS_DISABLED,
                    NoDrawReason::Repeated => ITEM_SOURCE_REPEATED,
                    NoDrawReason::UpdateOnly => ITEM_SOURCE_UPDATE_ONLY,
                };
                self.items.push(OwnedItem {
                    pass: self.current_pass(),
                    kind,
                    source_item: item as u64,
                });
                Ok(None)
            }
            PlanStep::Prepare { item, group } => {
                self.items.push(OwnedItem {
                    pass: self.current_pass(),
                    kind: ITEM_DRAW_REQUIRED,
                    source_item: item as u64,
                });
                Ok(Some(CallerWork::Prepare { item, group }))
            }
            PlanStep::Event(_) => {
                self.open = false;
                Ok(Some(CallerWork::Step(step)))
            }
            PlanStep::Transition { .. }
            | PlanStep::WaterRows
            | PlanStep::ObserveUpdateOnly { .. } => Ok(Some(CallerWork::Step(step))),
        }
    }

    /// Adds a draw to the open Main pass.
    pub fn add_draw(&mut self, draw: D) -> Result<(), EffectsError> {
        if !self.open {
            return Err(EffectsError::Association);
        }
        self.passes
            .last_mut()
            .expect("an open pass exists")
            .draws
            .push(draw);
        Ok(())
    }

    /// Adds a producer pass with its one draw and one producer item. Closes the open Main pass.
    pub fn add_producer_pass(&mut self, pass: &ProducerPass, draw: D) -> u64 {
        self.passes.push(OwnedPass {
            source: PassSource::Producer {
                target: pass.target,
            },
            colour: OwnedAttachment::from_image(pass.colour),
            depth: OwnedAttachment::from_image(pass.depth),
            stencil: stencil(pass.stencil),
            draws: vec![draw],
        });
        self.open = false;
        let identity = PRODUCER_IDENTITY | self.producer_items;
        self.producer_items += 1;
        self.items.push(OwnedItem {
            pass: self.current_pass(),
            kind: ITEM_DRAW_REQUIRED,
            source_item: identity,
        });
        identity
    }

    /// Adds retained clear passes on the given images. Closes the open Main pass.
    pub fn add_clear_passes(&mut self, clears: &[ClearPass], colour_image: u32, depth_image: u32) {
        for clear in clears {
            let colour = match clear.colour {
                ClearAttachment::Load => OwnedAttachment {
                    image: colour_image,
                    load: LOAD_LOAD,
                    store: STORE_STORE,
                    clear: [0.0; 4],
                },
                ClearAttachment::Clear(value) => OwnedAttachment {
                    image: colour_image,
                    load: LOAD_CLEAR,
                    store: STORE_STORE,
                    clear: value,
                },
            };
            let depth = match clear.depth {
                ClearAttachment::Load => OwnedAttachment {
                    image: depth_image,
                    load: LOAD_LOAD,
                    store: STORE_STORE,
                    clear: [0.0; 4],
                },
                ClearAttachment::Clear(value) => OwnedAttachment {
                    image: depth_image,
                    load: LOAD_CLEAR,
                    store: STORE_STORE,
                    clear: [value, 0.0, 0.0, 0.0],
                },
            };
            self.passes.push(OwnedPass {
                source: PassSource::RetainedClear,
                colour,
                depth,
                stencil: None,
                draws: Vec::new(),
            });
        }
        self.open = false;
    }
}

#[cfg(test)]
mod tests {
    use super::*;
    use mag_render_behavior::scene_plan::{
        DrawSources, ProducerEvent, SetupReferences, TraversalItem, traverse,
    };

    fn binding() -> SurfaceBinding {
        SurfaceBinding {
            address: 0x1000,
            physical: (320, 180),
            colour_format: 1,
            depth_format: 15,
            colour: Some(AttachmentImage {
                image: 3,
                load: LoadOp::Load,
            }),
            depth: Some(AttachmentImage {
                image: 4,
                load: LoadOp::Load,
            }),
            stencil: Some(AttachmentImage {
                image: 4,
                load: LoadOp::Load,
            }),
            depth_initialised: true,
            stencil_initialised: true,
        }
    }

    fn item(group: u32, first: u32) -> TraversalItem {
        let mut mbi = [0u8; 32];
        mbi[0] = (group << 4) as u8;
        let mut transform = [0u8; 112];
        transform[..4].copy_from_slice(&first.to_be_bytes());
        TraversalItem {
            mbi,
            transform: Some(transform),
            material: Some(Some(DrawSources {
                group,
                transform,
                pass_control: 3,
            })),
        }
    }

    #[test]
    fn plan_steps_become_passes_and_items() {
        let setup = SetupReferences {
            lists_enabled: true,
            initial: [0, 0x40, 0],
            group_thirteen: 0,
            group_fifteen: [0; 2],
        };
        let items = [
            item(0, 0x3f80_0000),
            item(0, 0x3f80_0000),
            item(12, 0x4000_0000),
        ];
        let (steps, _) = traverse(&items, &setup, 3).unwrap();
        let mut assembler = PacketAssembler::<&'static str>::new();
        let mut work = Vec::new();
        for step in steps {
            if let Some(w) = assembler.apply(step, &binding(), Some(1)).unwrap() {
                if let CallerWork::Prepare { .. } = w {
                    assembler.add_draw("draw").unwrap();
                }
                work.push(w);
            }
        }
        // Two Main passes: before item 0, and after the group-12 event.
        assert_eq!(assembler.passes.len(), 2);
        assert!(
            assembler
                .passes
                .iter()
                .all(|p| p.source == PassSource::Main { surface: 0x1000 })
        );
        let pass = &assembler.passes[0];
        assert_eq!(
            (pass.colour.image, pass.colour.load, pass.depth.image),
            (3, LOAD_LOAD, 4)
        );
        assert_eq!(pass.stencil.unwrap().load, LOAD_LOAD);
        let kinds: Vec<_> = assembler
            .items
            .iter()
            .map(|i| (i.pass, i.kind, i.source_item))
            .collect();
        assert_eq!(
            kinds,
            vec![
                (0, ITEM_DRAW_REQUIRED, 0),
                (0, ITEM_SOURCE_REPEATED, 1),
                (1, ITEM_DRAW_REQUIRED, 2)
            ]
        );
        assert_eq!(assembler.passes[0].draws, vec!["draw"]);
        assert!(
            work.contains(&CallerWork::Step(PlanStep::Event(ProducerEvent::Initial {
                slot: 1,
                target: 0x40
            })))
        );
        assert!(work.contains(&CallerWork::Step(PlanStep::WaterRows)));
        let abi = assembler.items[1].to_abi();
        assert_eq!(
            (
                abi.pass,
                abi.reserved,
                abi.source_item,
                abi.unresolved_obligations
            ),
            (0, ITEM_SOURCE_REPEATED, 1, 0)
        );
    }

    #[test]
    fn closed_pass_refuses_draws_and_unsupported_targets_refuse_to_open() {
        let mut assembler = PacketAssembler::<u32>::new();
        assert_eq!(assembler.add_draw(1), Err(EffectsError::Association));
        assert_eq!(
            assembler.apply(PlanStep::OpenPass, &binding(), Some(2)),
            Err(EffectsError::Unsupported("colour target"))
        );
        assembler
            .apply(PlanStep::OpenPass, &binding(), Some(1))
            .unwrap();
        assembler
            .apply(
                PlanStep::Event(ProducerEvent::Group12Textures),
                &binding(),
                Some(1),
            )
            .unwrap();
        assert_eq!(assembler.add_draw(1), Err(EffectsError::Association));
    }

    #[test]
    fn producer_passes_get_top_bit_identities() {
        use mag_render_behavior::effects::{FullscreenGeometry, ProducerViewport};
        use mag_render_behavior::surfaces::NativeViewport;
        let pass = ProducerPass {
            target: 0x2000,
            colour: None,
            depth: Some(AttachmentImage {
                image: 9,
                load: LoadOp::DontCare,
            }),
            stencil: Some(AttachmentImage {
                image: 9,
                load: LoadOp::Load,
            }),
            colour_format: 0,
            depth_format: 15,
            viewport: ProducerViewport {
                native: NativeViewport {
                    x: 0.0,
                    y: 0.0,
                    width: 1.0,
                    height: 1.0,
                    min_depth: 0.0,
                    max_depth: 1.0,
                    scissor: [0, 0, 1, 1],
                    z_scale: 0.5,
                    z_offset: 0.5,
                },
                shader_scale: [1.0, 1.0, 0.5, 0.0],
                shader_offset: [0.0, 0.0, 0.5, 0.0],
                clip: (1, 1),
            },
            geometry: FullscreenGeometry {
                vertices: Vec::new(),
                indices: vec![0, 1, 2],
                attribute0: [0; 4],
                attribute8: [0; 4],
            },
            state: Default::default(),
            textures: Vec::new(),
        };
        let mut assembler = PacketAssembler::<u32>::new();
        let a = assembler.add_producer_pass(&pass, 1);
        let b = assembler.add_producer_pass(&pass, 2);
        assert_eq!((a, b), (PRODUCER_IDENTITY, PRODUCER_IDENTITY | 1));
        let p = &assembler.passes[0];
        assert_eq!(p.source, PassSource::Producer { target: 0x2000 });
        assert_eq!(p.colour, OwnedAttachment::ABSENT);
        assert_eq!((p.depth.image, p.depth.load), (9, LOAD_DONT_CARE));
        assert_eq!(p.colour.to_abi().image, u32::MAX);
    }

    #[test]
    fn retained_clears_become_clear_passes() {
        use mag_render_behavior::scene_plan::{ClearState, ClearTarget, replay_clears};
        let target = ClearTarget {
            width: 8,
            height: 8,
            source_colour_format: 8,
            depth_format: 2,
            scissor: [0, 0, 8, 8],
        };
        let mut state = ClearState::default();
        let clears = replay_clears(
            &[
                (0, 0x1d90, 0xffff_0000),
                (0, 0x1d8c, 0xffff_ff00),
                (0, 0x1d94, 0xf1),
                (0, 0x1d94, 0x01),
            ],
            &target,
            &mut state,
        )
        .unwrap();
        let mut assembler = PacketAssembler::<u32>::new();
        assembler.add_clear_passes(&clears, 5, 6);
        assert_eq!(assembler.passes.len(), 2);
        let first = &assembler.passes[0];
        assert_eq!(
            (first.colour.load, first.colour.clear),
            (LOAD_CLEAR, [1.0, 0.0, 0.0, 1.0])
        );
        assert_eq!((first.depth.load, first.depth.clear[0]), (LOAD_CLEAR, 1.0));
        assert_eq!(assembler.passes[1].colour.load, LOAD_LOAD);
    }
}
