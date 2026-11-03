//! Ordered, single-writer compute prefix for already owned scene packets.
use super::*;
#[derive(Clone)]
pub struct Dispatch {
    pub program: u32,
    pub groups: [u32; 3],
    pub bindings: Vec<raw::Binding>,
}
impl ScenePacket {
    pub(super) fn validate_compute(&self) -> Result<(), i32> {
        if self.dispatches.len() > 32 {
            return Err(-5);
        }
        for (&index, &length) in &self.produced {
            if length == 0
                || self
                    .buffers
                    .get(index as usize)
                    .is_none_or(|b| !b.is_empty())
            {
                return Err(-1);
            }
        }
        for p in &self.programs {
            if p.stage == 2 {
                if p.flags != 0
                    || !p.textures.is_empty()
                    || !p.samplers.is_empty()
                    || !p.vertex_locations.is_empty()
                    || p.threads.contains(&0)
                    || p.threads
                        .into_iter()
                        .try_fold(1u32, |n, v| n.checked_mul(v))
                        .is_none_or(|n| n > 256)
                {
                    return Err(-1);
                }
            } else if p.stage > 2 {
                return Err(-1);
            }
        }
        for p in self.programs.iter().filter(|p| p.stage == 2) {
            let mut sizes = BTreeSet::new();
            for b in &p.buffers {
                if b.runtime_array_stride != 0
                    && (b.binding_kind != 0
                        || b.minimum_bytes != u64::from(b.runtime_array_stride)
                        || !sizes.insert(b.runtime_size_index))
                {
                    return Err(-1);
                }
            }
            if sizes.iter().copied().ne(0..sizes.len() as u32) {
                return Err(-1);
            }
        }
        let mut written = BTreeSet::new();
        for (node, d) in self.dispatches.iter().enumerate() {
            let p = self
                .programs
                .get(d.program as usize)
                .filter(|p| p.stage == 2)
                .ok_or(-1)?;
            if d.groups.contains(&0) || d.bindings.len() != p.buffers.len() {
                return Err(-1);
            }
            let mut slots = BTreeSet::new();
            let mut reads = BTreeSet::new();
            let mut writes = BTreeSet::new();
            for b in &d.bindings {
                if b.stage != 2 || !slots.insert(b.slot) {
                    return Err(-1);
                }
                let schema = p.buffers.iter().find(|s| s.slot == b.slot).ok_or(-1)?;
                self.range(b.view)?;
                if b.view.length < schema.minimum_bytes
                    || (schema.runtime_array_stride != 0
                        && !b
                            .view
                            .length
                            .is_multiple_of(u64::from(schema.runtime_array_stride)))
                    || !b
                        .view
                        .offset
                        .is_multiple_of(u64::from(schema.alignment_bytes))
                {
                    return Err(-1);
                }
                if schema.access == 2 {
                    let length = self.produced.get(&b.view.buffer).ok_or(-1)?;
                    if b.view.offset != 0
                        || b.view.length != *length
                        || written.contains(&b.view.buffer)
                        || !writes.insert(b.view.buffer)
                    {
                        return Err(-1);
                    }
                } else {
                    if self.produced.contains_key(&b.view.buffer)
                        && !written.contains(&b.view.buffer)
                    {
                        return Err(packet_failure(
                            self.source,
                            "validate/compute-read-before-write",
                            Some(node),
                            b.view.buffer,
                            -1,
                        ));
                    }
                    reads.insert(b.view.buffer);
                }
            }
            if reads.iter().any(|b| writes.contains(b)) || writes.is_empty() {
                return Err(-1);
            }
            written.extend(writes);
        }
        if written.len() != self.produced.len() {
            return Err(-1);
        }
        Ok(())
    }
}

impl ScenePacket {
    pub(super) fn compute_programs(&self) -> Vec<u32> {
        self.dispatches
            .iter()
            .map(|d| d.program)
            .collect::<BTreeSet<_>>()
            .into_iter()
            .collect()
    }
    pub(super) fn prepare_compute_prefix(
        &self,
        batch: &mut BatchBuilder,
        sources: &mut [Option<ReadSource>],
        allocations: &BTreeMap<u32, mag_render::batch::GpuBuffer>,
        ids: &[u64],
    ) -> Result<(), i32> {
        let programs = self.compute_programs();
        if ids.len() != self.pipelines.len() + programs.len() {
            return Err(-1);
        }
        for (node, d) in self.dispatches.iter().enumerate() {
            let p = &self.programs[d.program as usize];
            let mut bindings = Vec::new();
            for b in &d.bindings {
                let schema = p.buffers.iter().find(|s| s.slot == b.slot).ok_or(-1)?;
                let range = self.range(b.view)?;
                bindings.push(if schema.access == 2 {
                    mag_render::batch::Binding::Write {
                        slot: b.slot,
                        buffer: *allocations.get(&b.view.buffer).ok_or(-1)?,
                        range,
                    }
                } else {
                    mag_render::batch::Binding::Read {
                        slot: b.slot,
                        source: sources[b.view.buffer as usize].ok_or(-1)?,
                        range,
                    }
                });
            }
            let program = programs.binary_search(&d.program).map_err(|_| -1)?;
            batch
                .dispatch(ids[self.pipelines.len() + program], bindings, d.groups)
                .map_err(|error| {
                    packet_failure(self.source, "scene/compute-dispatch", Some(node), error, -1)
                })?;
            for b in &d.bindings {
                if p.buffers.iter().any(|s| s.slot == b.slot && s.access == 2) {
                    sources[b.view.buffer as usize] = Some(ReadSource::Produced(
                        batch
                            .value(*allocations.get(&b.view.buffer).ok_or(-1)?)
                            .map_err(|_| -1)?,
                    ))
                }
            }
        }
        Ok(())
    }
}
#[cfg(test)]
pub(crate) mod tests {
    use super::super::*;
    pub(crate) fn copied_fixture() -> ScenePacket {
        copied_fixture_with_textures(true)
    }
    pub(crate) fn copied_fixture_with_textures(textures: bool) -> ScenePacket {
        let mut packet = packet_tests::fixture();
        if !textures {
            packet.passes[0].draws[0].textures.clear();
            packet.pipelines[0].textures.clear();
            packet.programs[1].textures.clear();
            packet.programs[1].samplers.clear();
            packet.views.clear();
            packet.view_dimensions.clear();
            packet.samplers.clear();
        }
        let mut program = packet.programs[0].clone();
        program.stage = 2;
        program.threads = [32, 1, 1];
        program.textures.clear();
        program.samplers.clear();
        program.vertex_locations.clear();
        program.buffers = vec![
            BindingLayout {
                slot: 0,
                access: 1,
                minimum_bytes: 16,
                runtime_array_stride: 0,
                runtime_size_index: u32::MAX,
                alignment_bytes: 4,
                binding_kind: 0,
            },
            BindingLayout {
                slot: 1,
                access: 2,
                minimum_bytes: 72,
                runtime_array_stride: 0,
                runtime_size_index: u32::MAX,
                alignment_bytes: 4,
                binding_kind: 0,
            },
        ];
        packet.programs.push(program);
        let view = |buffer, length| raw::BufferUse {
            buffer,
            reserved: 0,
            offset: 0,
            length,
        };
        let bindings = [
            raw::Binding {
                stage: 2,
                slot: 0,
                view: view(2, 16),
            },
            raw::Binding {
                stage: 2,
                slot: 1,
                view: view(0, 72),
            },
        ];
        let dispatch = raw::Dispatch {
            program: 2,
            binding_count: 2,
            bindings: bindings.as_ptr(),
            groups: [1, 1, 1],
            reserved: 0,
        };
        packet_tests::copy_view_with_prefix(&packet, &[dispatch], &[0])
    }
    #[test]
    fn synchronous_packet_accepts_compute_output_as_the_actual_draw_vertex_buffer() {
        let copied = copied_fixture();
        assert_eq!(copied.produced.get(&0), Some(&72));
        assert!(copied.buffers[0].is_empty());
        assert_eq!(copied.dispatches.len(), 1);
        assert_eq!(copied.passes[0].draws[0].vertices.buffer, 0);
        assert_eq!(copied.passes[0].draws[0].vertices.length, 72);
    }

    struct Catalog<'a> {
        packet: &'a ScenePacket,
        bindings: Vec<BindingRequirement>,
    }
    impl PipelineCatalog for Catalog<'_> {
        fn owner_generation(&self) -> u64 {
            42
        }
        fn bindings(&self, id: u64) -> Option<&[BindingRequirement]> {
            (id == 2).then_some(self.bindings.as_slice())
        }
    }
    impl SceneCatalog for Catalog<'_> {
        fn scene_raster(&self, id: u64) -> Option<raw::RasterState> {
            packet_tests::Catalog(self.packet).scene_raster(id)
        }
        fn scene_graphics(&self, id: u64) -> Option<GraphicsLayout> {
            packet_tests::Catalog(self.packet).scene_graphics(id)
        }
    }
    #[test]
    fn actual_packet_preparation_preserves_compute_to_graphics_producer_reference() {
        let packet = copied_fixture();
        let catalog = Catalog {
            packet: &packet,
            bindings: packet.programs[2]
                .buffers
                .iter()
                .map(|b| BindingRequirement {
                    slot: b.slot,
                    access: if b.access == 1 {
                        mag_render::batch::Access::Read
                    } else {
                        mag_render::batch::Access::ReadWrite
                    },
                    minimum_bytes: b.minimum_bytes,
                    alignment: u64::from(b.alignment_bytes),
                })
                .collect(),
        };
        let prepared = packet.prepare(&catalog, &[1, 2], 32, 1 << 20).unwrap();
        let (commands, leases) = prepared.into_submission();
        assert_eq!(commands.compute().dispatches().len(), 1);
        assert_eq!(commands.compute().buffers().len(), 1);
        assert_eq!(leases.len(), 4);
        let ReadSource::Produced(value) = commands.passes()[0].draws[0].vertices else {
            panic!("produced vertices must not become an upload")
        };
        assert_eq!(commands.passes()[0].draws[0].vertex_range.length, 72);
        assert!(commands.compute().dispatches()[0].bindings.iter().any(|b|matches!(b,mag_render::batch::Binding::Write{buffer,range,..}if *buffer==value.buffer()&&range.length==72)));
        assert!(matches!(
            commands.passes()[0].draws[0].indices,
            ReadSource::Upload(_)
        ));
    }
    #[test]
    fn prefix_refuses_missing_multiple_partial_or_unordered_writers_and_produced_indices() {
        let original = copied_fixture();
        let mut invalid_uniform = original.clone();
        invalid_uniform.programs[2].buffers[1].binding_kind = 1;
        assert_eq!(invalid_uniform.validate(32, 1 << 20), Err(-1));

        let mut packet = original.clone();
        packet.dispatches.clear();
        assert_eq!(packet.validate(32, 1 << 20), Err(-1));
        let mut packet = original.clone();
        packet.dispatches.push(packet.dispatches[0].clone());
        assert_eq!(packet.validate(32, 1 << 20), Err(-1));
        let mut packet = original.clone();
        packet.dispatches[0].bindings[1].view.length = 68;
        packet.programs[2].buffers[1].minimum_bytes = 4;
        assert_eq!(packet.validate(32, 1 << 20), Err(-1));
        let mut packet = original.clone();
        packet.dispatches[0].bindings[1].view.buffer = 2;
        assert_eq!(packet.validate(32, 1 << 20), Err(-1));
        let mut packet = original.clone();
        packet.dispatches[0].bindings[0].view.buffer = 0;
        assert_eq!(packet.validate(32, 1 << 20), Err(-1));
        let mut packet = original.clone();
        packet.passes[0].draws[0].indices.buffer = 0;
        assert_eq!(packet.validate(32, 1 << 20), Err(-2));
        let mut packet = original.clone();
        packet.dispatches[0].bindings[1].view.offset = u64::MAX;
        assert_eq!(packet.validate(32, 1 << 20), Err(-1));
        let mut packet = original;
        packet.dispatches.resize(33, packet.dispatches[0].clone());
        assert_eq!(packet.validate(32, 1 << 20), Err(-5));
    }

    #[derive(Default)]
    struct Compiler {
        next: u64,
        compute: usize,
        graphics: usize,
        released: Vec<u64>,
    }
    impl PipelineCompiler for Compiler {
        fn compile(&mut self, _: &Pipeline, _: &[Program]) -> Result<u64, i32> {
            self.next += 1;
            self.graphics += 1;
            Ok(self.next)
        }
        fn compile_compute(&mut self, _: &Program) -> Result<u64, i32> {
            self.next += 1;
            self.compute += 1;
            Ok(self.next)
        }
        fn release(&mut self, id: u64) -> Result<(), i32> {
            self.released.push(id);
            Ok(())
        }
    }
    #[test]
    fn compute_cache_uses_full_program_metadata_and_shares_admission_pins() {
        let mut packet = copied_fixture();
        let mut cache = PipelineCache::default();
        let mut compiler = Compiler::default();
        let first = cache.resolve(&mut compiler, &packet).unwrap();
        assert_eq!(first.len(), 2);
        assert_eq!((compiler.graphics, compiler.compute), (1, 1));
        assert_eq!(cache.resolve(&mut compiler, &packet).unwrap(), first);
        assert_eq!((compiler.graphics, compiler.compute), (1, 1));
        cache.pin(&first).unwrap();
        packet.programs[2].threads = [64, 1, 1];
        let second = cache.resolve(&mut compiler, &packet).unwrap();
        assert_eq!(first[0], second[0]);
        assert_ne!(first[1], second[1]);
        packet.programs[2].source.push(b'x');
        let third = cache.resolve(&mut compiler, &packet).unwrap();
        assert_ne!(second[1], third[1]);
        assert!(compiler.released.is_empty());
        cache.unpin(&first).unwrap();
    }
}
