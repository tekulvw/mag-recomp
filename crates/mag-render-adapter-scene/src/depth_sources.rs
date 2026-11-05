//! Actual retained family3 Depth input population and shared raw asset ownership.
//! No original child descriptors, completed GPU payloads, draw or completion is
//! manufactured. The separate Depth source lease must cover every callback.
use crate::{
    decoded_geometry_cache::{self, RowCodec},
    depth_materials::{DepthMaterialError, OwnedDepthMaterials, copy_depth_materials},
    error::Error,
    geometry_attributes::{self, OwnedPrimaryAttributes},
    geometry_streams::GeometryStreamError,
    guest_memory::GuestSnapshot,
    material_sources::selected_resource_addresses,
    mesh_sources::{GeometrySource, MeshSource, PartitionChain},
    shared_scene_sources::{SourceContent, SourceDomain, SourceSnapshot},
};
use std::{cmp::Ordering, sync::Arc};

#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub struct DepthIdentity {
    pub vm: u64,
    pub invocation: u64,
    pub owner: u64,
    pub occurrence: u64,
    pub execution_update: u64,
    pub allocation: u64,
    pub job: u32,
    pub camera: u32,
}
#[derive(Clone, Copy, Debug)]
pub struct DepthInput {
    pub identity: DepthIdentity,
    pub bucket: u32,
    pub marker: u32,
    pub pass: u32,
    pub user: u32,
}
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub enum DepthError {
    Identity,
    Range,
    Budget,
    Read,
    Catalog,
    Population,
    Unsorted,
    InitialTie,
    Asset,
    MissingTransform,
    PartitionChain,
    StreamControl { slot: usize, control: u32 },
}
#[derive(Clone, Copy)]
pub struct DepthBudget {
    pub bytes: usize,
    pub reads: usize,
    pub items: usize,
}
impl Default for DepthBudget {
    fn default() -> Self {
        Self {
            bytes: 128 * 1024 * 1024,
            reads: 1048576,
            items: 262144,
        }
    }
}
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub enum DepthItemRoute {
    Prepare,
    Repeated,
    Terminates,
    AfterTermination,
}
#[derive(Debug)]
pub struct DepthItem {
    pub shard: u8,
    pub source_index: u16,
    pub mbi: [u8; 32],
    pub transform: Option<[u8; 112]>,
    pub route: DepthItemRoute,
    pub material: Option<Arc<SourceContent>>,
    pub mesh: Option<Arc<SourceContent>>,
    pub geometry: Option<GeometrySource>,
    /// Same immutable component records consumed by Main; deformation and
    /// Depth output binding remain separate pending responsibilities.
    pub immutable_partitions: Option<Result<Vec<DepthImmutablePartition>, DepthError>>,
}
#[derive(Debug)]
pub struct DepthImmutablePartition {
    pub source_address: u32,
    /// The partition record itself, as copied for the chain.
    pub record: Box<[u8; 544]>,
    pub positions: Result<Arc<Vec<[u32; 4]>>, GeometryStreamError>,
    pub indices: Result<Arc<Vec<u16>>, GeometryStreamError>,
    pub attributes: Option<Result<OwnedPrimaryAttributes, GeometryStreamError>>,
    /// The secondary attribute banks, when [`DepthCaptureOptions::secondary_attributes`] asks for
    /// them and the record declares some.
    pub secondary_attributes: Option<Result<OwnedPrimaryAttributes, DeformationSourceError>>,
    /// The palette-skinning sources of a skinned-partition item, when
    /// [`DepthCaptureOptions::skinning`] asks for them.
    pub skinning: Option<Result<DepthSkinningSource, DeformationSourceError>>,
}
/// Which deformation sources a capture copies beyond the positions, indices and primary banks.
///
/// Both are off by default, so a default capture reads exactly what it always has. The extra
/// reads come from the same budget and catalog; a failure is kept with its partition and never
/// fails the capture.
#[derive(Clone, Copy, Debug, Default, PartialEq, Eq)]
pub struct DepthCaptureOptions {
    /// The secondary attribute banks (record sizes at +0x0A, format stream 21, source stream 10).
    pub secondary_attributes: bool,
    /// For skinned-partition items: the skinning configuration (stream 19), the two palette
    /// segments and the influence stream (5).
    pub skinning: bool,
}
/// The palette-skinning sources of one partition, as copied.
#[derive(Debug)]
pub struct DepthSkinningSource {
    /// The skinning configuration stream (19).
    pub config: Arc<SourceContent>,
    /// The item's palette address plus the record's two 16-bit offsets at +0x14.
    pub palette_addresses: [u32; 2],
    /// The two palette segments, sized by the slot 3 and 4 controls.
    pub segments: [Arc<SourceContent>; 2],
    /// The influence stream (5), as its control sizes it.
    pub influences: Arc<SourceContent>,
}
/// Why a partition's optional deformation sources were not copied.
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub enum DeformationSourceError {
    Source(DepthError),
    Codec(GeometryStreamError),
    /// More than the first secondary input chunk is declared.
    SplitInputChunks([u16; 3]),
    /// The configuration selects a built-in secondary layout.
    Builtin(u8),
    /// The item carries no palette address.
    MissingPalette,
}
#[derive(Debug)]
pub struct OwnedDepthSources {
    pub identity: DepthIdentity,
    pub pass: Arc<SourceContent>,
    pub user: Arc<SourceContent>,
    /// Every actual list occurrence, including the source terminating tail.
    pub items: Vec<DepthItem>,
    /// Effective selected assets are retained in the SAME source window and
    /// catalog. Errors remain source obligations; they never authorize a draw.
    pub materials: Result<OwnedDepthMaterials, DepthMaterialError>,
    pub source_snapshot: Arc<SourceSnapshot>,
    pub copied_bytes: usize,
    pub reads: usize,
    /// Input capture is not a native Depth plan. These consumers are still open.
    pub pending: [DepthPending; 4],
}
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub enum DepthPending {
    EffectiveMaterialAndAlpha,
    SharedPreparedGeometry,
    DepthNormalAttachments,
    TerminalConsumers,
}
fn word(b: &[u8], at: usize) -> u32 {
    u32::from_be_bytes(b[at..at + 4].try_into().expect("checked source extent"))
}
fn priority(group: u8) -> u8 {
    match group {
        1..=6 => group,
        11 => 6,
        7 => 8,
        _ => 15,
    }
}
fn key(mbi: &[u8; 32]) -> [u8; 16] {
    let group = mbi[0] >> 4;
    let first = (u32::from(priority(group)) << 28)
        | if matches!(group, 1 | 7) {
            word(mbi, 0) & 0x0fff_fc00
        } else {
            0
        };
    let mut result = [0; 16];
    result[..4].copy_from_slice(&first.to_be_bytes());
    result[4..].copy_from_slice(&mbi[4..16]);
    result
}
struct Reader<'a, F> {
    read: &'a mut F,
    remaining: DepthBudget,
    snapshot: SourceSnapshot,
    bytes: usize,
    reads: usize,
}
impl<F: FnMut(u32, usize) -> Result<Vec<u8>, Error>> Reader<'_, F> {
    fn copy(&mut self, address: u32, bytes: usize) -> Result<Arc<SourceContent>, DepthError> {
        if address == 0
            || u64::from(address)
                .checked_add(bytes as u64)
                .is_none_or(|end| end > 1u64 << 32)
        {
            return Err(DepthError::Range);
        }
        if bytes > self.remaining.bytes || self.remaining.reads == 0 {
            return Err(DepthError::Budget);
        }
        let owned = (self.read)(address, bytes).map_err(|_| DepthError::Read)?;
        if owned.len() != bytes {
            return Err(DepthError::Read);
        }
        self.remaining.bytes -= bytes;
        self.remaining.reads -= 1;
        self.bytes += bytes;
        self.reads += 1;
        self.snapshot
            .observe(SourceDomain::Guest, address, &owned)
            .map_err(|_| DepthError::Catalog)
    }
}
/// No read is elided based on a guest address. The shared catalog interns only
/// exact freshly-owned bytes, so later Main can retain the same immutable content.
pub fn copy_depth_sources(
    input: DepthInput,
    budget: DepthBudget,
    read: &mut impl FnMut(u32, usize) -> Result<Vec<u8>, Error>,
) -> Result<OwnedDepthSources, DepthError> {
    copy_depth_sources_with(input, budget, DepthCaptureOptions::default(), read)
}
/// [`copy_depth_sources`], also copying the deformation sources `options` asks for.
pub fn copy_depth_sources_with(
    input: DepthInput,
    budget: DepthBudget,
    options: DepthCaptureOptions,
    read: &mut impl FnMut(u32, usize) -> Result<Vec<u8>, Error>,
) -> Result<OwnedDepthSources, DepthError> {
    let id = input.identity;
    if id.vm == 0
        || id.invocation == 0
        || id.owner == 0
        || id.occurrence == 0
        || id.execution_update == 0
        || id.allocation == 0
        || id.job == 0
        || id.camera > 3
    {
        return Err(DepthError::Identity);
    }
    let mut reader = Reader {
        read,
        remaining: budget,
        snapshot: SourceSnapshot::default(),
        bytes: 0,
        reads: 0,
    };
    let pass = reader.copy(input.pass, 688)?;
    let user = reader.copy(input.user, 160)?;
    let bucket = reader.copy(input.bucket, 0x68)?;
    let marker = reader.copy(input.marker, 320)?;
    let pass_bytes = pass.bytes();
    let bucket_bytes = bucket.bytes();
    let marker = marker.bytes();
    if u32::from(u16::from_be_bytes(
        pass_bytes[0x19a..0x19c].try_into().unwrap(),
    )) != id.camera
        || word(pass_bytes, 0xe4) != word(bucket_bytes, 4)
        || word(pass_bytes, 0xe0) != word(bucket_bytes, 12)
    {
        return Err(DepthError::Identity);
    }
    let mut shards: [Vec<DepthItem>; 4] = std::array::from_fn(|_| Vec::new());
    let mut total = 0usize;
    for (shard, items) in shards.iter_mut().enumerate() {
        let counts = &marker[80 * shard..80 * shard + 80];
        let population = (0..16)
            .try_fold(0usize, |n, g| n.checked_add(word(counts, 4 * g) as usize))
            .ok_or(DepthError::Population)?;
        if population != word(counts, 64) as usize || population > 65536 {
            return Err(DepthError::Population);
        }
        total = total
            .checked_add(population)
            .filter(|n| *n <= budget.items)
            .ok_or(DepthError::Budget)?;
        if population == 0 {
            continue;
        }
        // Depth reads the family-3 lists; a Shadow run (cohort camera slot 1
        // or 2) reads its own family's lists, laid out the same way.
        let family = if matches!(id.camera, 1 | 2) {
            id.camera as usize
        } else {
            3
        };
        let indices = reader.copy(
            word(bucket_bytes, 0x28 + family * 16 + 4 * shard),
            population * 2,
        )?;
        items
            .try_reserve_exact(population)
            .map_err(|_| DepthError::Budget)?;
        for index in indices.bytes().as_chunks::<2>().0 {
            let source_index = u16::from_be_bytes(*index);
            let address = word(pass_bytes, 0xe4)
                .checked_add(u32::from(source_index) * 32)
                .ok_or(DepthError::Range)?;
            let source = reader.copy(address, 32)?;
            let mbi: [u8; 32] = source.bytes().try_into().unwrap();
            let selected = selected_resource_addresses(&mbi, id.camera, word(pass_bytes, 0xe0));
            let transform = match selected {
                Ok(s) => Some(
                    reader
                        .copy(s.transform_address, 112)?
                        .bytes()
                        .try_into()
                        .unwrap(),
                ),
                Err(crate::material_sources::SourceError::MissingCameraTransform) => None,
                Err(_) => return Err(DepthError::Asset),
            };
            let item = DepthItem {
                shard: shard as u8,
                source_index,
                mbi,
                transform,
                route: DepthItemRoute::AfterTermination,
                material: None,
                mesh: None,
                geometry: None,
                immutable_partitions: None,
            };
            if items
                .last()
                .is_some_and(|old| key(&old.mbi) > key(&item.mbi))
            {
                return Err(DepthError::Unsorted);
            }
            items.push(item);
        }
        // Marker words are retained as source metadata. Their group-label
        // convention is not borrowed from Main; only the explicit total and
        // complete source list population are used by this reader.
    }
    // Source family3 merge reinserts only the consumed list, retaining ties.
    // Initial equal heads require a separately source-qualified priority order.
    for a in 0..4 {
        for b in a + 1..4 {
            if shards[a]
                .first()
                .zip(shards[b].first())
                .is_some_and(|(x, y)| key(&x.mbi) == key(&y.mbi))
            {
                return Err(DepthError::InitialTie);
            }
        }
    }
    let mut streams = shards.map(Vec::into_iter);
    let mut heads = std::array::from_fn::<_, 4, _>(|i| streams[i].next());
    let compare = |a: usize, b: usize, h: &[Option<DepthItem>; 4]| match (&h[a], &h[b]) {
        (Some(a), Some(b)) => key(&a.mbi).cmp(&key(&b.mbi)),
        (Some(_), None) => Ordering::Less,
        (None, Some(_)) => Ordering::Greater,
        _ => Ordering::Equal,
    };
    let mut order = [0usize, 1, 2, 3];
    order.sort_by(|a, b| compare(*a, *b, &heads));
    let mut items = Vec::new();
    items
        .try_reserve_exact(total)
        .map_err(|_| DepthError::Budget)?;
    let mut prior: Option<(u32, [u8; 112])> = None;
    let mut terminated = false;
    while items.len() < total {
        let first = order[0];
        let mut item = heads[first].take().ok_or(DepthError::Population)?;
        if !terminated {
            if priority(item.mbi[0] >> 4) == 15 {
                item.route = DepthItemRoute::Terminates;
                terminated = true;
            } else {
                let transform = item
                    .transform
                    .as_ref()
                    .ok_or(DepthError::MissingTransform)?;
                let addresses =
                    selected_resource_addresses(&item.mbi, id.camera, word(pass_bytes, 0xe0))
                        .map_err(|_| DepthError::Asset)?;
                let repeated = prior.as_ref().is_some_and(|(mesh, p)| {
                    *mesh == addresses.mesh_address
                        && p[..32]
                            .as_chunks::<4>()
                            .0
                            .iter()
                            .zip(transform[..32].as_chunks::<4>().0)
                            .all(|(a, b)| {
                                let a = u32::from_be_bytes(*a);
                                let b = u32::from_be_bytes(*b);
                                a == b || ((a | b) & 0x7f80_0000) == 0
                            })
                });
                if repeated {
                    item.route = DepthItemRoute::Repeated;
                } else {
                    item.route = DepthItemRoute::Prepare;
                    let material =
                        reader.copy(addresses.material_address, addresses.material_bytes)?;
                    let mesh = reader.copy(addresses.mesh_address, 192)?;
                    let snapshot = GuestSnapshot::new(addresses.mesh_address, mesh.bytes())
                        .map_err(|_| DepthError::Asset)?;
                    let decoded = MeshSource::copy(&snapshot, addresses.mesh_address)
                        .map_err(|_| DepthError::Asset)?;
                    let flags = material.bytes().get(0x58..0x5c).ok_or(DepthError::Asset)?;
                    item.geometry = Some(decoded.depth_geometry_source(
                        u32::from_be_bytes(flags.try_into().unwrap()),
                        &item.mbi,
                        word(pass_bytes, 0x284),
                    ));
                    if decoded_geometry_cache::enabled() {
                        item.immutable_partitions = Some(capture_immutable_partitions(
                            &mut reader,
                            item.geometry.as_ref().expect("qualified dispatch"),
                            options,
                        ));
                    }
                    item.material = Some(material);
                    item.mesh = Some(mesh);
                    prior = Some((addresses.mesh_address, *transform));
                }
            }
        }
        items.push(item);
        heads[first] = streams[first].next();
        let mut at = 0;
        while at < 3 && compare(order[at + 1], first, &heads) == Ordering::Less {
            at += 1;
        }
        order[..=at].rotate_left(1);
    }
    // Reserve decoded metadata from the existing total budget before allowing
    // further encoded reads. This is not an independent extra cache allowance.
    // Unused reservation is returned; an error drops its temporary allocations.
    let reserved = (reader.remaining.bytes / 2).min(16 * 1024 * 1024);
    reader.remaining.bytes -= reserved;
    let materials = copy_depth_materials(pass.bytes(), &items, reserved, &mut |a, n| {
        reader.copy(a, n)
    });
    let retained = materials.as_ref().map_or(0, |m| m.decoded_bytes);
    reader.remaining.bytes += reserved - retained;
    Ok(OwnedDepthSources {
        identity: id,
        pass,
        user,
        items,
        materials,
        source_snapshot: Arc::new(reader.snapshot),
        copied_bytes: reader.bytes,
        reads: reader.reads,
        pending: [
            DepthPending::EffectiveMaterialAndAlpha,
            DepthPending::SharedPreparedGeometry,
            DepthPending::DepthNormalAttachments,
            DepthPending::TerminalConsumers,
        ],
    })
}

fn capture_immutable_partitions<F: FnMut(u32, usize) -> Result<Vec<u8>, Error>>(
    reader: &mut Reader<'_, F>,
    geometry: &GeometrySource,
    options: DepthCaptureOptions,
) -> Result<Vec<DepthImmutablePartition>, DepthError> {
    let first = match geometry {
        GeometrySource::StaticPartitions { first, .. }
        | GeometrySource::SkinnedPartitions { first, .. } => *first,
        GeometrySource::DirectDraw => return Ok(Vec::new()),
    };
    let mut source_error = None;
    let chain =
        PartitionChain::copy_with_reader(first, reader.remaining.reads.min(65536), &mut |a| {
            reader
                .copy(a, 544)
                .map(|v| v.bytes().to_vec())
                .map_err(|e| {
                    source_error = Some(e);
                    Error::OutsideSnapshot
                })
        })
        .map_err(|_| source_error.unwrap_or(DepthError::PartitionChain))?;
    let mut result = Vec::new();
    result
        .try_reserve_exact(chain.partitions().len())
        .map_err(|_| DepthError::Budget)?;
    for partition in chain.partitions() {
        let p = partition.bytes();
        let config_stream = stream(reader, p, 19)?;
        let config = config_stream.bytes().get(..16).ok_or(DepthError::Asset)?;
        let source = stream(reader, p, 7)?;
        let index = stream(reader, p, 1)?;
        let format = if config[2] == 255 {
            Some(stream(reader, p, 20)?)
        } else {
            None
        };
        let vertices = usize::from(u16::from_be_bytes([config[8], config[9]]));
        let count = usize::from(u16::from_be_bytes([config[10], config[11]]));
        let output = vertices
            .checked_mul(16)
            .and_then(|n| n.checked_add(count * 2))
            .filter(|n| *n <= reader.remaining.bytes)
            .ok_or(DepthError::Budget)?;
        reader.remaining.bytes -= output;
        let codec = match config[2] {
            0 => Ok(RowCodec::Float32x3),
            6 => Ok(RowCodec::Rebased16x3),
            255 => decoded_geometry_cache::position_codec(
                format.as_ref().unwrap().bytes(),
                word(p, 0x34 + 8 * 7),
            ),
            other => Err(GeometryStreamError::UnsupportedPositionEncoding(other)),
        };
        let positions = codec.and_then(|codec| {
            decoded_geometry_cache::rows(
                codec,
                format.as_ref().map_or(&[], |f| f.bytes()),
                source.bytes(),
                vertices,
                output,
            )
        });
        let indices = decoded_geometry_cache::indices(index.bytes(), count, output);
        let attributes = format.map(|f| {
            let decoded = geometry_attributes::decode_primary_attributes(
                f.bytes(),
                source.bytes(),
                word(p, 0x34 + 8 * 7),
                vertices,
                reader.remaining.bytes,
            );
            if let Ok(attributes) = &decoded {
                reader.remaining.bytes -= attributes.owned_bytes()
            }
            decoded
        });
        let secondary_attributes = if options.secondary_attributes {
            secondary(reader, p, config, vertices)
        } else {
            None
        };
        let skinning = match (options.skinning, geometry) {
            (
                true,
                GeometrySource::SkinnedPartitions {
                    palette_address, ..
                },
            ) => Some(skinning(
                reader,
                p,
                *palette_address,
                Arc::clone(&config_stream),
            )),
            _ => None,
        };
        result.push(DepthImmutablePartition {
            source_address: partition.source_address,
            record: Box::new(*p),
            positions,
            indices,
            attributes,
            secondary_attributes,
            skinning,
        });
    }
    Ok(result)
}

/// One stream of a partition record, as its slot's control sizes it.
fn stream<F: FnMut(u32, usize) -> Result<Vec<u8>, Error>>(
    reader: &mut Reader<'_, F>,
    p: &[u8; 544],
    slot: usize,
) -> Result<Arc<SourceContent>, DepthError> {
    let control = word(p, 0x30 + 8 * slot);
    if control >> 16 != 0 {
        return Err(DepthError::StreamControl { slot, control });
    }
    if control == 0 {
        return crate::shared_scene_sources::retain_content(&[]).map_err(|_| DepthError::Catalog);
    }
    reader.copy(word(p, 0x34 + 8 * slot), control as usize)
}

/// The secondary attribute banks a record declares: the first input chunk only, through a
/// described (not built-in) layout.
fn secondary<F: FnMut(u32, usize) -> Result<Vec<u8>, Error>>(
    reader: &mut Reader<'_, F>,
    p: &[u8; 544],
    config: &[u8],
    vertices: usize,
) -> Option<Result<OwnedPrimaryAttributes, DeformationSourceError>> {
    use DeformationSourceError as E;
    let sizes: [u16; 3] =
        core::array::from_fn(|i| u16::from_be_bytes([p[0x0a + 2 * i], p[0x0b + 2 * i]]));
    if sizes == [0; 3] {
        return None;
    }
    Some((|| {
        if sizes[1..].iter().any(|&size| size != 0) {
            return Err(E::SplitInputChunks(sizes));
        }
        if config[3] != 255 {
            return Err(E::Builtin(config[3]));
        }
        let format = stream(reader, p, 21).map_err(E::Source)?;
        let address = word(p, 0x34 + 8 * 10);
        let source = reader
            .copy(address, usize::from(sizes[0]))
            .map_err(E::Source)?;
        let decoded = geometry_attributes::decode_primary_attributes(
            format.bytes(),
            source.bytes(),
            address,
            vertices,
            reader.remaining.bytes,
        )
        .map_err(E::Codec)?;
        reader.remaining.bytes -= decoded.owned_bytes();
        Ok(decoded)
    })())
}

/// The palette-skinning sources of one skinned partition.
fn skinning<F: FnMut(u32, usize) -> Result<Vec<u8>, Error>>(
    reader: &mut Reader<'_, F>,
    p: &[u8; 544],
    palette_address: u32,
    config: Arc<SourceContent>,
) -> Result<DepthSkinningSource, DeformationSourceError> {
    use DeformationSourceError as E;
    if palette_address == 0 {
        return Err(E::MissingPalette);
    }
    let palette_addresses: [u32; 2] = core::array::from_fn(|i| {
        palette_address.wrapping_add(u32::from(u16::from_be_bytes([
            p[0x14 + 2 * i],
            p[0x15 + 2 * i],
        ])))
    });
    let mut segment = |i: usize| -> Result<Arc<SourceContent>, DepthError> {
        let slot = 3 + i;
        let control = word(p, 0x30 + 8 * slot);
        if control >> 16 != 0 {
            return Err(DepthError::StreamControl { slot, control });
        }
        if control == 0 {
            return crate::shared_scene_sources::retain_content(&[])
                .map_err(|_| DepthError::Catalog);
        }
        reader.copy(palette_addresses[i], control as usize)
    };
    let segments = [
        segment(0).map_err(E::Source)?,
        segment(1).map_err(E::Source)?,
    ];
    let influences = stream(reader, p, 5).map_err(E::Source)?;
    Ok(DepthSkinningSource {
        config,
        palette_addresses,
        segments,
        influences,
    })
}
