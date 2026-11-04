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
pub fn classify(method: u32) -> Option<MethodClass> {
    use MethodClass as C;
    if method & 3 != 0 {
        return None;
    }
    Some(match method {
        0x0100 => C::NoOperation,
        0x0050 | 0x0064 | 0x0068 | 0x006c | 0x1d6c | 0x1d70 | 0x1d74 | 0x1fd8 => C::Synchronization,
        0x1d94 => C::Clear,
        0x1808 => C::Primitive,
        0x180c..=0x1818 => C::Draw,
        0x0200..=0x02fc | 0x08c0 | 0x08c4 | 0x0a00..=0x0a5c => C::Attachment,
        0x0b80..=0x0bfc | 0x08e4 | 0x1d60 | 0x1d64 | 0x1e9c | 0x1ea0 | 0x1ef8 | 0x1ff0..=0x1ff8 => {
            C::Program
        }
        0x1efc | 0x1f00..=0x1f7c => C::VertexConstants,
        0x1680..=0x17bc | 0x181c | 0x1820 | 0x1e00..=0x1e40 | 0x1fc0 => C::VertexInput,
        0x1a00..=0x1bfc | 0x1840..=0x187c | 0x0b00..=0x0b3c | 0x1d00..=0x1d3c => C::TextureBinding,
        0x0300..=0x03a0
        | 0x0a60..=0x0a7c
        | 0x1830
        | 0x1834
        | 0x1838
        | 0x183c
        | 0x1d78
        | 0x1d7c
        | 0x1d88
        | 0x1d8c
        | 0x1d90
        | 0x1fec => C::Raster,
        _ => return None,
    })
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
