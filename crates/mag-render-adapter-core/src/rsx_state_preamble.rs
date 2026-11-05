//! State-only replay of recorded RSX method writes.
//!
//! A command list recorded from the game can mix render state with work:
//! draws, clears, primitive brackets and synchronisation. When the frontend
//! has already done that work natively, but the RSX commands that follow it
//! still need the state the list would have left, the state alone has to be
//! replayed as real guest methods.
//!
//! [`encode_state_preamble`] takes the recorded writes in their original order
//! and keeps only the requested state classes. It drops work methods and
//! no-operations, and refuses anything it cannot classify. The result is a
//! callable command block: it ends in a return.
//!
//! Writes are kept in source order, so a program upload keeps the order of its
//! load-slot and data writes. Consecutive writes to increasing methods are
//! merged into one incrementing packet, which the RSX executes as the same
//! sequence of single writes.
//!
//! The method classes are NV4097 (3D class, subchannel 0) interface facts. A
//! write on another subchannel is refused, because its meaning depends on the
//! object bound there. Nothing here runs unless a host calls it.

/// What a method write does, as far as a state replay is concerned.
#[derive(Clone, Copy, Debug, PartialEq, Eq, Hash)]
pub enum MethodClass {
    /// Vertex program upload, load slot and start; fragment program address
    /// and control; program-related control words.
    Program,
    /// Vertex constant load slot and constant data.
    VertexConstants,
    /// Vertex array formats and offsets, array bases, index array, frequency
    /// divider and the other vertex-input words.
    VertexInput,
    /// Texture unit descriptors, filters, controls and address words.
    TextureBinding,
    /// Fixed-function raster state: blending, colour masks, depth, stencil,
    /// alpha test, cull, polygon offset, clear values and similar.
    Raster,
    /// Surface and render-target setup, viewport and scissor.
    Attachment,
    /// Work: a clear.
    Clear,
    /// Work: begin or end of a primitive.
    Primitive,
    /// Work: draw data (array ranges, index ranges, inline vertices).
    Draw,
    /// Semaphores, references, labels and similar synchronisation.
    Synchronization,
    /// No-operation.
    NoOperation,
}

impl MethodClass {
    /// Whether the class is render state (replayable) rather than work.
    pub fn is_state(self) -> bool {
        matches!(
            self,
            Self::Program
                | Self::VertexConstants
                | Self::VertexInput
                | Self::TextureBinding
                | Self::Raster
                | Self::Attachment
        )
    }
}

/// Classify an NV4097 method offset (subchannel 0). `None` when the method is
/// not one this replay knows.
///
/// The table follows the NV4097 method register list. Arrays cover all their
/// elements (16 vertex attributes, 16 texture units, 4 vertex texture units).
/// Context DMA bindings select memory objects and are replayed as state.
pub fn classify(method: u32) -> Option<MethodClass> {
    use MethodClass as C;
    if method & 3 != 0 {
        return None;
    }
    Some(match method {
        0x0100 => C::NoOperation,
        // Channel semaphores and references, notify, idle wait, performance
        // trigger, report clear and get, semaphore releases, L2 invalidate.
        0x0050
        | 0x0060..=0x006c
        | 0x0104
        | 0x0110
        | 0x0140
        | 0x17c8
        | 0x1800
        | 0x1d6c
        | 0x1d70
        | 0x1d74
        | 0x1fd8 => C::Synchronization,
        // Surface clear and zcull surface clear.
        0x1d94 | 0x1438 => C::Clear,
        0x1808 => C::Primitive,
        // Array elements, draw arrays, inline array, draw index array.
        0x180c..=0x1818 | 0x1824 => C::Draw,
        // Texture A/B and vertex A/B context DMAs.
        0x0184 | 0x0188 => C::TextureBinding,
        0x019c | 0x01a0 => C::VertexInput,
        // The other context DMAs: notifies, colour A-D, state, zeta,
        // semaphore, report, clip ID and cull data.
        0x0180..=0x01b8 => C::Attachment,
        // Surface, window clip, scissor, viewport, surface compression.
        0x0200..=0x02fc | 0x08c0 | 0x08c4 | 0x0a00..=0x0a5c | 0x1d80 => C::Attachment,
        // Transform program, shader program and control, constant read
        // limits, execution mode, program load and start, timeout, output
        // attribute routing, input and output masks, branch bits.
        0x0b80..=0x0bfc
        | 0x08e4
        | 0x1d60
        | 0x1d64
        | 0x1e94
        | 0x1e9c
        | 0x1ea0
        | 0x1ef8
        | 0x1fc4..=0x1fd4
        | 0x1ff0..=0x1ff8 => C::Program,
        0x1efc | 0x1f00..=0x1f7c => C::VertexConstants,
        // Immediate vertex data (scaled4s, 3f, 2f, 2s, 4ub, 4s, 4f, 1f),
        // array offsets, base offset and index, formats, index array, restart
        // index, frequency divider.
        0x0a80..=0x0afc
        | 0x1500..=0x15bc
        | 0x1680..=0x17bc
        | 0x181c
        | 0x1820
        | 0x1880..=0x19fc
        | 0x1c00..=0x1cfc
        | 0x1dac
        | 0x1db0
        | 0x1e00..=0x1e7c
        | 0x1fc0 => C::VertexInput,
        // Anisotropic spread, vertex textures, texture control 2 and 3,
        // texture units, colour keys, no-paranoid fetches.
        0x03c0..=0x03fc
        | 0x0900..=0x097c
        | 0x0b00..=0x0b3c
        | 0x1840..=0x187c
        | 0x1a00..=0x1bfc
        | 0x1d00..=0x1d3c
        | 0x1fe8 => C::TextureBinding,
        // Fixed-function raster state.
        0x0300..=0x03a0
        | 0x03b0
        | 0x03b8
        | 0x03bc
        | 0x08cc..=0x08d4
        | 0x0a60..=0x0a7c
        | 0x0b40..=0x0b7c
        | 0x1428
        | 0x142c
        | 0x1450
        | 0x1454
        | 0x145c
        | 0x1478
        | 0x147c
        | 0x1480..=0x14fc
        | 0x17cc
        | 0x1804
        | 0x1828..=0x183c
        | 0x1d78
        | 0x1d7c
        | 0x1d84..=0x1d90
        | 0x1d98
        | 0x1d9c
        | 0x1da4
        | 0x1db4
        | 0x1db8
        | 0x1e98
        | 0x1ea4..=0x1eac
        | 0x1ee0..=0x1ee8
        | 0x1fe0
        | 0x1fec => C::Raster,
        _ => return None,
    })
}

/// Every distinct subchannel-0 method among `writes` that [`classify`] does
/// not know, with how many writes use it, in method order. A diagnostic for a
/// refused replay: one pass names all of them, not just the first.
pub fn unclassified_methods(writes: &[MethodWrite]) -> Vec<(u32, usize)> {
    let mut found: Vec<(u32, usize)> = Vec::new();
    for w in writes.iter().filter(|w| w.subchannel == 0) {
        if classify(w.method).is_some() {
            continue;
        }
        match found.binary_search_by_key(&w.method, |&(m, _)| m) {
            Ok(at) => found[at].1 += 1,
            Err(at) => found.insert(at, (w.method, 1)),
        }
    }
    found
}

/// One recorded method write.
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub struct MethodWrite {
    pub subchannel: u8,
    pub method: u32,
    pub value: u32,
}

/// A set of [`MethodClass`]es to keep.
#[derive(Clone, Copy, Debug, Default, PartialEq, Eq)]
pub struct ClassSet(u16);
impl ClassSet {
    fn bit(class: MethodClass) -> u16 {
        1 << class as u16
    }
    pub const fn empty() -> Self {
        Self(0)
    }
    /// Every state class.
    pub fn all_state() -> Self {
        [
            MethodClass::Program,
            MethodClass::VertexConstants,
            MethodClass::VertexInput,
            MethodClass::TextureBinding,
            MethodClass::Raster,
            MethodClass::Attachment,
        ]
        .into_iter()
        .fold(Self::empty(), Self::with)
    }
    pub fn with(self, class: MethodClass) -> Self {
        Self(self.0 | Self::bit(class))
    }
    pub fn contains(self, class: MethodClass) -> bool {
        self.0 & Self::bit(class) != 0
    }
}

#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub enum PreambleError {
    /// The keep set names a work class (only state can be replayed).
    WorkClass(MethodClass),
    /// A write on a subchannel other than 0, at this index of the input.
    Subchannel { index: usize, subchannel: u8 },
    /// A method this replay cannot classify, at this index of the input.
    Unclassified { index: usize, method: u32 },
    /// The block (including its return) would exceed this many words.
    Capacity { words: usize },
}

/// NV40 FIFO return.
pub const RETURN: u32 = 0x0002_0000;
/// Largest method count of one packet header.
const MAX_COUNT: usize = 0x7ff;

/// What an encoded block contains.
#[derive(Clone, Debug, PartialEq, Eq)]
pub struct StatePreamble {
    /// Command words, ending in [`RETURN`].
    pub words: Vec<u32>,
    /// Number of writes kept.
    pub kept: usize,
    /// Writes dropped as work, no-operations or classes not kept.
    pub dropped: usize,
}

/// Replay the state classes in `keep` from `writes`, in order, as a callable
/// command block of at most `capacity_words` words. Every write is classified
/// before any output, so a refusal leaves nothing half-encoded.
pub fn encode_state_preamble(
    writes: &[MethodWrite],
    keep: ClassSet,
    capacity_words: usize,
) -> Result<StatePreamble, PreambleError> {
    for class in [
        MethodClass::Clear,
        MethodClass::Primitive,
        MethodClass::Draw,
        MethodClass::Synchronization,
        MethodClass::NoOperation,
    ] {
        if keep.contains(class) {
            return Err(PreambleError::WorkClass(class));
        }
    }
    let mut kept_writes = Vec::with_capacity(writes.len());
    for (index, write) in writes.iter().enumerate() {
        if write.subchannel != 0 {
            return Err(PreambleError::Subchannel {
                index,
                subchannel: write.subchannel,
            });
        }
        let class = classify(write.method).ok_or(PreambleError::Unclassified {
            index,
            method: write.method,
        })?;
        if keep.contains(class) {
            kept_writes.push(*write);
        }
    }
    let mut words = Vec::new();
    let mut i = 0;
    while i < kept_writes.len() {
        let first = kept_writes[i].method;
        let mut n = 1;
        while i + n < kept_writes.len()
            && n < MAX_COUNT
            && kept_writes[i + n].method == first + 4 * n as u32
        {
            n += 1;
        }
        words.push(((n as u32) << 18) | first);
        words.extend(kept_writes[i..i + n].iter().map(|w| w.value));
        i += n;
    }
    words.push(RETURN);
    if words.len() > capacity_words {
        return Err(PreambleError::Capacity { words: words.len() });
    }
    Ok(StatePreamble {
        words,
        kept: kept_writes.len(),
        dropped: writes.len() - kept_writes.len(),
    })
}
