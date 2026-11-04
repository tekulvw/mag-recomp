//! `ProducerResources` over exact image provenance.

use mag_render_behavior::effects::{PacketImageInfo, ProducerTextureBinding, SurfaceBinding};
use mag_render_behavior::producer::{ImageInfo, ProducerResources, TargetInfo};
use mag_render_behavior::surfaces::DepthSource;
use std::collections::HashMap;

/// Packet formats that are depth formats.
fn is_depth_format(format: u32) -> bool {
    matches!(format, 2 | 15 | 16)
}

type DepthKey = (u32, u32, u32, u32, u32);

fn depth_key(source: &DepthSource) -> DepthKey {
    (
        source.location as u32,
        source.offset,
        source.pitch,
        source.width,
        source.height,
    )
}

/// Images and targets known to the packet being built, looked up only by exact keys.
#[derive(Clone, Debug, Default)]
pub struct ImageTable {
    images: HashMap<(u32, [u32; 8]), ImageInfo>,
    depth_planes: HashMap<DepthKey, ImageInfo>,
    targets: HashMap<u32, TargetInfo>,
}

impl ImageTable {
    pub fn new() -> Self {
        Self::default()
    }

    /// Records an image whose provenance is exactly (`address`, `descriptor`).
    pub fn insert_image(&mut self, address: u32, descriptor: [u32; 8], image: ImageInfo) {
        self.images.insert((address, descriptor), image);
    }

    /// Records a validated producer texture binding and its packet image.
    pub fn insert_producer_binding(
        &mut self,
        binding: &ProducerTextureBinding,
        image: &PacketImageInfo,
    ) {
        let mip_count = binding.words[1] >> 16;
        self.insert_image(
            binding.address,
            binding.words,
            ImageInfo {
                id: binding.image,
                width: image.width,
                height: image.height,
                mip_count,
                samples: image.samples,
                layers: image.layers,
                two_dimensional: image.dimension == 2,
                depth: is_depth_format(image.format),
            },
        );
    }

    /// Records the image of a classified depth plane.
    pub fn insert_depth_plane(&mut self, source: &DepthSource, image: ImageInfo) {
        self.depth_planes.insert(depth_key(source), image);
    }

    /// Records the images behind a surface record. `rsx_colour_format` and `rsx_depth_format`
    /// are the record's surface formats; the binding was checked consistent with the record.
    pub fn insert_target(
        &mut self,
        binding: &SurfaceBinding,
        rsx_colour_format: u32,
        rsx_depth_format: u32,
    ) {
        let (width, height) = binding.physical;
        let colour_image = binding.colour.map(|colour| ImageInfo {
            id: colour.image,
            width,
            height,
            mip_count: 1,
            samples: 1,
            layers: 1,
            two_dimensional: true,
            depth: false,
        });
        self.targets.insert(
            binding.address,
            TargetInfo {
                physical: binding.physical,
                colour_format: rsx_colour_format,
                colour_image,
                colour_initialised: binding.colour.is_some(),
                depth_format: rsx_depth_format,
                depth_initialised: binding.depth_initialised,
                stencil_initialised: binding.stencil_initialised,
            },
        );
    }

    /// Marks planes of a target written by a pass of this packet.
    pub fn mark_written(&mut self, address: u32, colour: bool, depth: bool, stencil: bool) {
        if let Some(target) = self.targets.get_mut(&address) {
            target.colour_initialised |= colour;
            target.depth_initialised |= depth;
            target.stencil_initialised |= stencil;
        }
    }
}

impl ProducerResources for ImageTable {
    fn image(&self, source_address: u32, descriptor: &[u32; 8]) -> Option<ImageInfo> {
        self.images.get(&(source_address, *descriptor)).copied()
    }

    fn depth_plane(&self, source: &DepthSource) -> Option<ImageInfo> {
        self.depth_planes.get(&depth_key(source)).copied()
    }

    fn target(&self, record_address: u32) -> Option<TargetInfo> {
        self.targets.get(&record_address).copied()
    }
}

#[cfg(test)]
mod tests {
    use super::*;
    use mag_render_behavior::effects::{AttachmentImage, LoadOp};
    use mag_render_behavior::surfaces::{DepthPlaneKind, Location};

    const WORDS: [u32; 8] = [0x2000, 0x0001_a522, 0, 0, 0, 0, 0x00a0_005a, 0];

    #[test]
    fn exact_provenance_only() {
        let mut table = ImageTable::new();
        let binding = ProducerTextureBinding {
            address: 0x3120,
            words: WORDS,
            image: 7,
            format: 13,
            width: 160,
            height: 90,
        };
        let image = PacketImageInfo {
            format: 13,
            width: 160,
            height: 90,
            dimension: 2,
            samples: 1,
            layers: 1,
        };
        table.insert_producer_binding(&binding, &image);
        let found = table.image(0x3120, &WORDS).unwrap();
        assert_eq!(
            (found.id, found.width, found.mip_count, found.depth),
            (7, 160, 1, false)
        );
        // A different address or any different word is not the same image.
        assert!(table.image(0x3140, &WORDS).is_none());
        let mut other = WORDS;
        other[5] = 1;
        assert!(table.image(0x3120, &other).is_none());
        let depth = PacketImageInfo {
            format: 15,
            ..image
        };
        table.insert_producer_binding(
            &ProducerTextureBinding {
                address: 0x3140,
                ..binding
            },
            &depth,
        );
        assert!(table.image(0x3140, &WORDS).unwrap().depth);
    }

    #[test]
    fn targets_and_depth_planes() {
        let mut table = ImageTable::new();
        let binding = SurfaceBinding {
            address: 0x1000,
            physical: (640, 180),
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
            stencil: None,
            depth_initialised: false,
            stencil_initialised: false,
        };
        table.insert_target(&binding, 8, 2);
        let target = table.target(0x1000).unwrap();
        assert_eq!(
            (target.physical, target.colour_format, target.depth_format),
            ((640, 180), 8, 2)
        );
        assert_eq!(target.colour_image.unwrap().id, 3);
        assert!(!target.depth_initialised);
        table.mark_written(0x1000, false, true, false);
        assert!(table.target(0x1000).unwrap().depth_initialised);
        assert!(table.target(0x2000).is_none());
        let source = DepthSource {
            kind: DepthPlaneKind::NormalisedDepth,
            location: Location::Local,
            offset: 0x40_0000,
            pitch: 5120,
            width: 1280,
            height: 720,
            owner: None,
        };
        let image = ImageInfo {
            id: 9,
            width: 1280,
            height: 720,
            mip_count: 1,
            samples: 1,
            layers: 1,
            two_dimensional: true,
            depth: true,
        };
        table.insert_depth_plane(&source, image);
        assert_eq!(table.depth_plane(&source), Some(image));
        assert_eq!(
            table.depth_plane(&DepthSource {
                pitch: 2560,
                ..source
            }),
            None
        );
    }
}
