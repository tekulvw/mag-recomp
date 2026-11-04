//! Synchronous all-item raw asset acquisition for a published Main cohort.
//!
//! This is the bridge from actual selected visibility records to the authored
//! material/mesh/partition decoders. Returned resources own bytes; this module
//! does not manufacture a guest lifetime, cache generation, shader or draw.
use crate::{
    cohort::OwnedVisibilityCohort,
    error::Error,
    guest_memory::GuestSnapshot,
    material_sources::{
        MaterialSource, SourceError, TechniqueResourceBase, TechniqueSource,
        selected_resource_addresses,
    },
    mesh_sources::{MainGeometrySource, MeshSource, MeshSourceError, PartitionChain},
    partition_inputs::StreamRequest,
};

#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub struct MainPassResources {
    /// Source pass +DC, interpreted as signed when selecting the technique.
    pub render_mode: i32,
    /// Source pass +194, not a generic feature-admission mask.
    pub technique_override_mask: u32,
    /// Source pass +284, an address base, never an allocation generation.
    pub geometry_resource_base: u32,
}
/// One immutable pass record supplied by the source owner for this capture.
/// This borrow is data, never a guest pointer, asset epoch or lifetime lease.
#[derive(Clone, Copy)]
pub struct MainPassSnapshot<'a> {
    address: u32,
    bytes: &'a [u8; 688],
}
impl<'a> MainPassSnapshot<'a> {
    pub fn new(address: u32, bytes: &'a [u8]) -> Result<Self, Error> {
        if address == 0 || !address.is_multiple_of(4) {
            return Err(Error::InvalidRange);
        }
        GuestSnapshot::new(address, bytes)?;
        Ok(Self {
            address,
            bytes: bytes.try_into().map_err(|_| Error::InvalidRange)?,
        })
    }
    pub fn address(self) -> u32 {
        self.address
    }
    pub fn bytes(self) -> &'a [u8; 688] {
        self.bytes
    }
    pub fn slice(self, address: u32, bytes: usize) -> Option<&'a [u8]> {
        let low = usize::try_from(address.checked_sub(self.address)?).ok()?;
        self.bytes.get(low..low.checked_add(bytes)?)
    }
}
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub struct ResourceCopyBudget {
    pub bytes: usize,
    pub reads: usize,
    pub partition_headers: usize,
}
#[derive(Clone, Debug, PartialEq, Eq)]
pub enum CohortResourceError {
    BudgetExceeded,
    AllocationFailed,
    SourceRange(StreamRequest),
    Read {
        request: StreamRequest,
        error: Error,
    },
    ReadExtent {
        request: StreamRequest,
        actual: usize,
    },
    Material(SourceError),
    Mesh(MeshSourceError),
}
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub enum ResourceObligation {
    MissingCameraTransform,
    SourceReadWindowAndResourceOwnership,
    SecondaryMaterialSelection,
    TextureAndLightingState,
    VertexAttributesAndDeformation,
    NativeShaderAndDrawState,
    PassTargetAndSubmission,
}
pub struct OwnedTechniqueChunk {
    pub source: TechniqueSource,
    pub bytes: Vec<u8>,
}
pub struct ItemAssetResources {
    /// Exact cohort order, including duplicates, without a selected-case filter.
    pub ordinal: usize,
    pub primary_material: Option<MaterialSource>,
    pub mesh: Option<MeshSource>,
    pub primary_technique: Option<OwnedTechniqueChunk>,
    pub geometry_source: Option<MainGeometrySource>,
    pub partitions: Option<PartitionChain>,
    pub obligations: Vec<ResourceObligation>,
}
pub struct OwnedCohortResources {
    pub vm: u64,
    pub source_invocation: u64,
    pub execution_update: u64,
    pub pass_occurrence: u64,
    pub items: Vec<ItemAssetResources>,
    pub copied_bytes: usize,
    pub reads: usize,
}
struct Reader<'a, F> {
    remaining: ResourceCopyBudget,
    copied: usize,
    reads: usize,
    read: &'a mut F,
}
impl<F: FnMut(StreamRequest) -> Result<Vec<u8>, Error>> Reader<'_, F> {
    fn copy(&mut self, address: u32, bytes: usize) -> Result<Vec<u8>, CohortResourceError> {
        let request = StreamRequest { address, bytes };
        if address == 0
            || u64::from(address)
                .checked_add(bytes as u64)
                .is_none_or(|end| end > 1u64 << 32)
        {
            return Err(CohortResourceError::SourceRange(request));
        }
        if bytes > self.remaining.bytes || self.remaining.reads == 0 {
            return Err(CohortResourceError::BudgetExceeded);
        }
        let owned =
            (self.read)(request).map_err(|error| CohortResourceError::Read { request, error })?;
        if owned.len() != bytes {
            return Err(CohortResourceError::ReadExtent {
                request,
                actual: owned.len(),
            });
        }
        self.remaining.bytes -= bytes;
        self.remaining.reads -= 1;
        self.copied += bytes;
        self.reads += 1;
        Ok(owned)
    }
}
/// The caller must execute this within the source producer's actual read
/// window. A callback is a synchronous copying mechanism, never readiness proof.
/// Native local copy identities may be allocated later; guest EA reuse is never
/// treated as resource identity. An error returns no partially usable graph.
pub fn copy_cohort_resources(
    cohort: &OwnedVisibilityCohort,
    pass: MainPassResources,
    budget: ResourceCopyBudget,
    read: &mut impl FnMut(StreamRequest) -> Result<Vec<u8>, Error>,
) -> Result<OwnedCohortResources, CohortResourceError> {
    let mut reader = Reader {
        remaining: budget,
        copied: 0,
        reads: 0,
        read,
    };
    let mut items = Vec::new();
    items
        .try_reserve_exact(cohort.items().len())
        .map_err(|_| CohortResourceError::AllocationFailed)?;
    for (ordinal, item) in cohort.items().iter().enumerate() {
        let mut result = ItemAssetResources {
            ordinal,
            primary_material: None,
            mesh: None,
            primary_technique: None,
            geometry_source: None,
            partitions: None,
            obligations: vec![
                ResourceObligation::SourceReadWindowAndResourceOwnership,
                ResourceObligation::SecondaryMaterialSelection,
                ResourceObligation::TextureAndLightingState,
                ResourceObligation::VertexAttributesAndDeformation,
                ResourceObligation::NativeShaderAndDrawState,
                ResourceObligation::PassTargetAndSubmission,
            ],
        };
        let Some(transform) = item.transform.as_ref() else {
            result
                .obligations
                .push(ResourceObligation::MissingCameraTransform);
            items.push(result);
            continue;
        };
        // Only the material/mesh locations are used. The transform is already
        // owned by the actual cohort; no new read of a derived transform occurs.
        let selected = selected_resource_addresses(&item.mbi, cohort.header().camera, 0)
            .map_err(CohortResourceError::Material)?;
        let material_bytes = reader.copy(selected.material_address, selected.material_bytes)?;
        let material = MaterialSource::from_owned(
            selected.material_address,
            material_bytes,
            selected.material_bytes,
        )
        .map_err(CohortResourceError::Material)?;
        let mesh_bytes = reader.copy(selected.mesh_address, 192)?;
        let snapshot = GuestSnapshot::new(selected.mesh_address, &mesh_bytes).map_err(|error| {
            CohortResourceError::Read {
                request: StreamRequest {
                    address: selected.mesh_address,
                    bytes: 192,
                },
                error,
            }
        })?;
        let mesh = MeshSource::copy(&snapshot, selected.mesh_address)
            .map_err(CohortResourceError::Mesh)?;
        let technique = material
            .select_technique(
                item.mbi[4],
                mesh.material_group(),
                pass.render_mode,
                pass.technique_override_mask,
                TechniqueResourceBase::Primary,
            )
            .map_err(CohortResourceError::Material)?;
        let chunk = reader.copy(technique.source_address, technique.transfer_bytes as usize)?;
        let flags = u32::from_be_bytes(
            material
                .bytes()
                .get(0x58..0x5c)
                .ok_or(CohortResourceError::Material(SourceError::MaterialBounds))?
                .try_into()
                .map_err(|_| CohortResourceError::Material(SourceError::MaterialBounds))?,
        );
        let dispatch = mesh.main_geometry_source(
            flags,
            &item.mbi,
            transform[0x6f],
            pass.geometry_resource_base,
        );
        let first = match dispatch {
            MainGeometrySource::GroupZeroPartitions { first, .. }
            | MainGeometrySource::StaticPartitions { first, .. }
            | MainGeometrySource::SkinnedPartitions { first, .. } => Some(first),
            MainGeometrySource::DirectDraw { .. } => None,
        };
        if let Some(first) = first {
            // The sparse reader owns each source header before traversing its
            // next link. Preserve the true chain population and source errors.
            let mut copy_error = None;
            let chain = PartitionChain::copy_with_reader(
                first,
                reader.remaining.partition_headers,
                &mut |address| {
                    reader.copy(address, 544).map_err(|error| {
                        copy_error = Some(error);
                        Error::OutsideSnapshot
                    })
                },
            );
            if let Some(error) = copy_error {
                return Err(error);
            }
            let chain = chain.map_err(CohortResourceError::Mesh)?;
            reader.remaining.partition_headers -= chain.partitions().len();
            result.partitions = Some(chain);
        }
        result.geometry_source = Some(dispatch);
        result.primary_technique = Some(OwnedTechniqueChunk {
            source: technique,
            bytes: chunk,
        });
        result.primary_material = Some(material);
        result.mesh = Some(mesh);
        items.push(result);
    }
    Ok(OwnedCohortResources {
        vm: cohort.header().vm,
        source_invocation: cohort.header().source_invocation,
        execution_update: cohort.header().execution_update,
        pass_occurrence: cohort.header().pass_occurrence,
        items,
        copied_bytes: reader.copied,
        reads: reader.reads,
    })
}
