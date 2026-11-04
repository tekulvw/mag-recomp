//! Owned RSX texture source storage, independently of packet preparation.
//! Unknown formats fail; source offsets never stand in for allocation versions.
use crate::error::Error;
use std::{collections::HashMap, sync::Arc};

#[derive(Clone, Debug, PartialEq, Eq)]
pub struct OwnedDescriptorSource {
    /// Source address only; not an allocation/content generation.
    pub address: u32,
    pub bytes: Vec<u8>,
}
/// One 32-byte texture binding record of a material, with the target word,
/// status and descriptor it names when its route is the ordinary object route.
#[derive(Clone, Debug, PartialEq, Eq)]
pub struct TextureObjectSource {
    pub record_index: usize,
    pub record: [u8; 32],
    /// Only the ordinary object route uses this descriptor target. Producer and
    /// null-object routes retain their raw record without dereferencing unused
    /// fields or inventing a target/inherited sampler state.
    pub target_word: Option<u32>,
    pub status: Option<[u8; 16]>,
    pub descriptor: Option<[u8; 32]>,
}

#[derive(Clone, Copy, Debug, PartialEq, Eq)]
#[repr(u32)]
pub enum TextureLocation {
    Local = 0,
    Main = 1,
}
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub struct LocatedTextureRequest {
    pub location: TextureLocation,
    pub offset: u32,
    pub bytes: usize,
}
#[derive(Clone, Debug, PartialEq, Eq)]
pub enum TextureError {
    Format(u8),
    Location(u8),
    Dimension(u8),
    Extent,
    Budget,
    Allocation,
    Marker,
    Disabled,
    MissingDescriptor,
    MissingSource,
    Unit(u32),
    UnqualifiedRoute(u8),
    Source(Error),
}
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub enum NativeTextureFormat {
    R8Unorm,
    Rg8Unorm,
    Rgba8Unorm,
    Bc1RgbaUnorm,
    Bc2RgbaUnorm,
    Bc3RgbaUnorm,
    R16Unorm,
    Rg16Unorm,
    Rgba16Float,
    Rgba32Float,
    R32Float,
    Rg16Float,
}
#[derive(Clone, Debug, PartialEq, Eq)]
pub struct TextureLevelLayout {
    pub face: u32,
    pub level: u32,
    pub width: u32,
    pub height: u32,
    pub source_offset: usize,
    pub source_row_bytes: usize,
    pub output_bytes: usize,
}
#[derive(Clone, Debug, PartialEq, Eq)]
pub struct TextureLayout {
    location: TextureLocation,
    offset: u32,
    source_bytes: usize,
    output_bytes: usize,
    format: NativeTextureFormat,
    levels: Vec<TextureLevelLayout>,
    descriptor: [u32; 8],
    source_bpp: usize,
    output_bpp: usize,
    block_bytes: usize,
    source_format: u8,
    swizzled: bool,
}
impl TextureLayout {
    pub fn location(&self) -> TextureLocation {
        self.location
    }
    pub fn offset(&self) -> u32 {
        self.offset
    }
    pub fn source_bytes(&self) -> usize {
        self.source_bytes
    }
    pub fn output_bytes(&self) -> usize {
        self.output_bytes
    }
    pub fn levels(&self) -> &[TextureLevelLayout] {
        &self.levels
    }
}
#[derive(Clone, Debug, PartialEq, Eq)]
pub struct TextureImageLevel {
    pub face: u32,
    pub level: u32,
    pub width: u32,
    pub height: u32,
    pub bytes: Vec<u8>,
}
#[derive(Clone, Debug, PartialEq, Eq)]
pub struct OwnedTextureImage {
    pub format: NativeTextureFormat,
    pub descriptor: [u32; 8],
    /// Exact copied source span, including any spacing between cube faces.
    pub source_bytes: Vec<u8>,
    pub levels: Vec<TextureImageLevel>,
}
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub enum DescriptorSelection {
    ResidentObject(u32),
    Fallback(usize),
    /// Actual retained producer descriptor EA, never an allocation generation.
    Producer(u32),
    /// Actual per-item producer descriptor selecting a compressed source
    /// asset. Its bytes are copied under this source window, not borrowed from
    /// a render-target cache or claimed to be a GPU allocation generation.
    ProducerAsset(u32),
}
/// BC1/2/3 cannot be color/depth attachments. Per-item descriptor setters may
/// select these ordinary source assets (notably pass1C4 lightmaps); use the
/// same bounded current source copy as resident material textures. Uncompressed
/// A5/color and depth aliases still require genuine native image association.
pub fn producer_asset_descriptor(descriptor: [u32; 8]) -> bool {
    matches!(((descriptor[1] >> 8) as u8) & !0x20, 0x86..=0x88)
}
/// Native producer descriptors may name current render output. CPU guest
/// bytes are not a version or visibility proof for that output. Preserve the
/// descriptor write and require the actual imported/generated native image.
#[derive(Clone, Debug, PartialEq, Eq)]
pub enum TexturePayload {
    Cpu(Result<Arc<OwnedTextureImage>, TextureError>),
    NativeProducer,
}
impl TexturePayload {
    pub fn cpu(&self) -> Option<&Result<Arc<OwnedTextureImage>, TextureError>> {
        match self {
            Self::Cpu(image) => Some(image),
            Self::NativeProducer => None,
        }
    }
}
#[derive(Clone, Debug, PartialEq, Eq)]
pub struct OwnedTextureBinding {
    pub record_index: usize,
    pub unit: u8,
    pub selection: DescriptorSelection,
    pub descriptor: [u32; 8],
    /// Compact source descriptors do not write BORDER_COLOR. A caller must
    /// supply actual prior state when its addressing mode observes the border.
    pub inherited_border_color: Option<u32>,
    pub image: TexturePayload,
}
/// Only successful immutable copies are shared, within this one capture. This
/// key is not reused across invocations or called a guest allocation version.
#[derive(Default)]
pub struct TextureImageCache {
    images: HashMap<[u32; 8], Arc<OwnedTextureImage>>,
}
/// Where a capture reads located texture spans. `validate` is the opt-in
/// copy-free path (`PS3_RENDER_CAPTURE_VIEW_VALIDATION=1`): it compares the
/// requested span in place with `expected` and, only when they are equal,
/// accounts the read exactly as `read` would (`Ok(true)`). `Ok(false)` means
/// nothing was accounted and the caller performs the ordinary `read`.
pub trait LocatedSource {
    fn read(&mut self, request: LocatedTextureRequest) -> Result<Vec<u8>, TextureError>;
    fn validate(
        &mut self,
        _request: LocatedTextureRequest,
        _expected: &[u8],
    ) -> Result<bool, TextureError> {
        Ok(false)
    }
}
/// A plain read closure: never validates in place.
pub struct ReadOnly<'a, F>(pub &'a mut F);
impl<F: FnMut(LocatedTextureRequest) -> Result<Vec<u8>, TextureError>> LocatedSource
    for ReadOnly<'_, F>
{
    fn read(&mut self, request: LocatedTextureRequest) -> Result<Vec<u8>, TextureError> {
        (self.0)(request)
    }
}
/// Copy one explicit source descriptor's immutable image. Shared with actual
/// per-item producer texture writes; failures consume the enclosing read budget
/// and cannot become an empty/default image. Cache identity is capture-local.
pub fn copy_texture_source_image(
    descriptor: [u32; 8],
    remaining_decoded_bytes: &mut usize,
    cache: &mut TextureImageCache,
    read: &mut impl FnMut(LocatedTextureRequest) -> Result<Vec<u8>, TextureError>,
) -> Result<Arc<OwnedTextureImage>, TextureError> {
    copy_texture_source_image_from(
        descriptor,
        remaining_decoded_bytes,
        cache,
        &mut ReadOnly(read),
    )
}
pub fn copy_texture_source_image_from(
    descriptor: [u32; 8],
    remaining_decoded_bytes: &mut usize,
    cache: &mut TextureImageCache,
    source: &mut impl LocatedSource,
) -> Result<Arc<OwnedTextureImage>, TextureError> {
    if let Some(image) = cache.images.get(&descriptor) {
        return Ok(Arc::clone(image));
    }
    let _timer =
        crate::source_acquisition::section(crate::source_acquisition::Section::TextureImages);
    let layout = texture_layout(descriptor, usize::MAX, *remaining_decoded_bytes)?;
    *remaining_decoded_bytes -= layout.output_bytes;
    let request = LocatedTextureRequest {
        location: layout.location,
        offset: layout.offset,
        bytes: layout.source_bytes,
    };
    // Opt-in copy-free reuse: the current span is compared in place with the
    // retained image's source bytes; equal bytes are the same reuse the copy
    // path below authorises, accounted as the same read.
    if crate::source_acquisition::view_validation_enabled()
        && let Some(candidate) = crate::source_acquisition::resident_candidate(descriptor)
        && source.validate(request, &candidate.source_bytes)?
    {
        crate::source_acquisition::confirm_resident(descriptor, &candidate);
        cache
            .images
            .try_reserve(1)
            .map_err(|_| TextureError::Allocation)?;
        cache.images.insert(descriptor, Arc::clone(&candidate));
        return Ok(candidate);
    }
    let bytes = source.read(request)?;
    // Opt-in: an image retained from an earlier update is reused only when this
    // update's freshly read span is byte-identical; otherwise decode as before.
    let image = match crate::source_acquisition::reuse_texture(descriptor, bytes) {
        Ok(image) => image,
        Err(bytes) => {
            let image = Arc::new(decode_texture_image(layout, bytes)?);
            crate::source_acquisition::remember_texture(&image);
            image
        }
    };
    cache
        .images
        .try_reserve(1)
        .map_err(|_| TextureError::Allocation)?;
    cache.images.insert(descriptor, Arc::clone(&image));
    Ok(image)
}
// When the primary material is also the selected one, a record whose route is
// 0..=2 or whose object word is zero writes no texture binding. The secondary
// material's fallback is a different source path and is not admitted here.
fn primary_texture_noop(source: &TextureObjectSource) -> bool {
    source.record[12] <= 2 || source.record[4..8] == [0; 4]
}
/// Associate a compact applied/refused result with the original raw record.
/// No-op raw records remain owned in the material's raw texture record list.
pub fn texture_binding_record_index(
    sources: &[TextureObjectSource],
    binding_index: usize,
) -> Option<usize> {
    sources
        .iter()
        .filter(|source| !primary_texture_noop(source))
        .nth(binding_index)
        .map(|source| source.record_index)
}
/// Resolve only the texture binding writes made when the primary material is
/// also the selected one. The caller must qualify that material role; the
/// secondary material's fallback is a different path.
/// Source no-op records remain owned in the enclosing raw request list and
/// leave prior texture state unchanged. Real failed writes retain typed errors.
pub fn copy_primary_texture_bindings(
    sources: &[TextureObjectSource],
    fallbacks: &[Option<OwnedDescriptorSource>; 6],
    remaining_decoded_bytes: &mut usize,
    cache: &mut TextureImageCache,
    read: &mut impl FnMut(LocatedTextureRequest) -> Result<Vec<u8>, TextureError>,
) -> Result<Vec<Result<OwnedTextureBinding, TextureError>>, TextureError> {
    copy_primary_texture_bindings_from(
        sources,
        fallbacks,
        remaining_decoded_bytes,
        cache,
        &mut ReadOnly(read),
    )
}
pub fn copy_primary_texture_bindings_from(
    sources: &[TextureObjectSource],
    fallbacks: &[Option<OwnedDescriptorSource>; 6],
    remaining_decoded_bytes: &mut usize,
    cache: &mut TextureImageCache,
    read: &mut impl LocatedSource,
) -> Result<Vec<Result<OwnedTextureBinding, TextureError>>, TextureError> {
    let mut output = Vec::new();
    reserve(&mut output, sources.len())?;
    for source in sources {
        if primary_texture_noop(source) {
            continue;
        }
        output.push((|| {
            let object = u32::from_be_bytes(source.record[4..8].try_into().unwrap());
            let status = source.status.ok_or(TextureError::MissingSource)?[0];
            let (selection, raw) = if status == 2 {
                (
                    DescriptorSelection::ResidentObject(object),
                    source.descriptor.ok_or(TextureError::MissingDescriptor)?,
                )
            } else {
                let (missing, pending) = match source.record[13] {
                    1 => (2, 3),
                    3 => (0, 1),
                    value => return Err(TextureError::Dimension(value)),
                };
                let index = if status == 1 { pending } else { missing };
                let descriptor = fallbacks[index]
                    .as_ref()
                    .ok_or(TextureError::MissingDescriptor)?;
                (
                    DescriptorSelection::Fallback(index),
                    descriptor
                        .bytes
                        .as_slice()
                        .try_into()
                        .map_err(|_| TextureError::Extent)?,
                )
            };
            let unit = (source.target_word.ok_or(TextureError::MissingDescriptor)? >> 14) & 1023;
            if unit >= 16 {
                return Err(TextureError::Unit(unit));
            }
            let descriptor = std::array::from_fn(|i| {
                u32::from_be_bytes(raw[i * 4..i * 4 + 4].try_into().unwrap())
            });
            let image =
                copy_texture_source_image_from(descriptor, remaining_decoded_bytes, cache, read);
            Ok(OwnedTextureBinding {
                record_index: source.record_index,
                unit: unit as u8,
                selection,
                descriptor,
                inherited_border_color: None,
                image: TexturePayload::Cpu(image),
            })
        })());
    }
    Ok(output)
}
fn reserve<T>(vec: &mut Vec<T>, count: usize) -> Result<(), TextureError> {
    vec.try_reserve_exact(count)
        .map_err(|_| TextureError::Allocation)
}
fn product(a: usize, b: usize) -> Result<usize, TextureError> {
    a.checked_mul(b).ok_or(TextureError::Extent)
}

/// Texture words are OFFSET,FORMAT,ADDRESS,CONTROL0,CONTROL1,FILTER,IMAGE_RECT,
/// CONTROL3. This is a storage/layout contract, not complete sampler admission.
pub fn texture_layout(
    descriptor: [u32; 8],
    source_budget: usize,
    output_budget: usize,
) -> Result<TextureLayout, TextureError> {
    let [offset, format, _, control0, _, _, rect, control3] = descriptor;
    if offset & 3 != 0 {
        return Err(TextureError::Marker);
    }
    if control0 & 0x80000000 == 0 {
        return Err(TextureError::Disabled);
    }
    let location = match format & 3 {
        1 => TextureLocation::Local,
        2 => TextureLocation::Main,
        value => return Err(TextureError::Location(value as u8)),
    };
    let dimension = ((format >> 4) & 15) as u8;
    if dimension != 2 {
        return Err(TextureError::Dimension(dimension));
    }
    let width = rect >> 16;
    let height = rect & 65535;
    let cube = format & 4 != 0;
    if width == 0 || height == 0 || (cube && width != height) || control3 >> 20 > 1 {
        return Err(TextureError::Extent);
    }
    let encoded_format = ((format >> 8) & 255) as u8;
    let source_format = encoded_format & 0x9f;
    use NativeTextureFormat::*;
    let (native, source_bpp, output_bpp, block_bytes) = match source_format {
        0x81 => (R8Unorm, 1, 1, 0),
        0x8b => (Rg8Unorm, 2, 2, 0),
        0x82 | 0x83 | 0x84 | 0x97 | 0x9d => (Rgba8Unorm, 2, 4, 0),
        0x85 | 0x9e => (Rgba8Unorm, 4, 4, 0),
        0x86 => (Bc1RgbaUnorm, 0, 0, 8),
        0x87 => (Bc2RgbaUnorm, 0, 0, 16),
        0x88 => (Bc3RgbaUnorm, 0, 0, 16),
        0x92 | 0x94 => (R16Unorm, 2, 2, 0),
        0x95 => (Rg16Unorm, 4, 4, 0),
        0x9a => (Rgba16Float, 8, 8, 0),
        0x9b => (Rgba32Float, 16, 16, 0),
        0x9c => (R32Float, 4, 4, 0),
        0x9f => (Rg16Float, 4, 4, 0),
        value => return Err(TextureError::Format(value)),
    };
    let swizzled = block_bytes == 0 && encoded_format & 0x20 == 0;
    if swizzled && (!width.is_power_of_two() || !height.is_power_of_two()) {
        return Err(TextureError::Extent);
    }
    let level_count = (format >> 16) & 65535;
    let max_levels = 32 - width.max(height).leading_zeros();
    if level_count == 0 || level_count > max_levels {
        return Err(TextureError::Extent);
    }
    let faces = if cube { 6 } else { 1 };
    let mut levels = Vec::new();
    reserve(&mut levels, (faces * level_count) as usize)?;
    let mut face_bytes = 0usize;
    let mut one_face_output = 0usize;
    for level in 0..level_count {
        let w = (width >> level).max(1);
        let h = (height >> level).max(1);
        let row = if block_bytes != 0 {
            product(w.div_ceil(4) as usize, block_bytes)?
        } else {
            product(w as usize, source_bpp)?
        };
        let pitch = (control3 & 0xfffff) as usize;
        let row = if level == 0 && encoded_format & 0x20 != 0 && block_bytes == 0 {
            if pitch != 0 && pitch < row {
                return Err(TextureError::Extent);
            }
            row.max(pitch)
        } else {
            row
        };
        let source_level = product(
            row,
            if block_bytes != 0 {
                h.div_ceil(4) as usize
            } else {
                h as usize
            },
        )?;
        let output = if block_bytes != 0 {
            source_level
        } else {
            product(product(w as usize, h as usize)?, output_bpp)?
        };
        levels.push(TextureLevelLayout {
            face: 0,
            level,
            width: w,
            height: h,
            source_offset: face_bytes,
            source_row_bytes: row,
            output_bytes: output,
        });
        face_bytes = face_bytes
            .checked_add(source_level)
            .ok_or(TextureError::Extent)?;
        one_face_output = one_face_output
            .checked_add(output)
            .ok_or(TextureError::Extent)?;
    }
    let stride = face_bytes.checked_add(127).ok_or(TextureError::Extent)? & !127;
    for face in 1..faces {
        for level in 0..level_count as usize {
            let mut item = levels[level].clone();
            item.face = face;
            item.source_offset = item
                .source_offset
                .checked_add(product(face as usize, stride)?)
                .ok_or(TextureError::Extent)?;
            levels.push(item);
        }
    }
    let source_bytes = face_bytes
        .checked_add(product(faces as usize - 1, stride)?)
        .ok_or(TextureError::Extent)?;
    let output_bytes = product(one_face_output, faces as usize)?;
    if source_bytes > source_budget || output_bytes > output_budget {
        return Err(TextureError::Budget);
    }
    if u64::from(offset) + source_bytes as u64 > 1u64 << 32 {
        return Err(TextureError::Extent);
    }
    Ok(TextureLayout {
        location,
        offset,
        source_bytes,
        output_bytes,
        format: native,
        levels,
        descriptor,
        source_bpp,
        output_bpp,
        block_bytes,
        source_format,
        swizzled,
    })
}

fn morton(mut x: u32, mut y: u32, mut xb: u32, mut yb: u32) -> usize {
    let mut result = 0usize;
    let mut bit = 0;
    while xb != 0 || yb != 0 {
        if xb != 0 {
            result |= ((x & 1) as usize) << bit;
            x >>= 1;
            xb -= 1;
            bit += 1;
        }
        if yb != 0 {
            result |= ((y & 1) as usize) << bit;
            y >>= 1;
            yb -= 1;
            bit += 1;
        }
    }
    result
}
fn texel(format: u8, source: &[u8], out: &mut [u8]) {
    fn expand(value: u16, max: u16) -> u8 {
        (u32::from(value) * 255 / u32::from(max)) as u8
    }
    match format {
        0x85 | 0x9e => out.copy_from_slice(&[
            source[1],
            source[2],
            source[3],
            if format == 0x9e { 255 } else { source[0] },
        ]),
        0x82 | 0x83 | 0x84 | 0x97 | 0x9d => {
            let v = u16::from_be_bytes([source[0], source[1]]);
            let rgba = match format {
                0x82 | 0x9d => [
                    expand((v >> 10) & 31, 31),
                    expand((v >> 5) & 31, 31),
                    expand(v & 31, 31),
                    if format == 0x9d || v & 0x8000 != 0 {
                        255
                    } else {
                        0
                    },
                ],
                0x83 => [
                    expand((v >> 8) & 15, 15),
                    expand((v >> 4) & 15, 15),
                    expand(v & 15, 15),
                    expand(v >> 12, 15),
                ],
                0x84 => [
                    expand(v >> 11, 31),
                    expand((v >> 5) & 63, 63),
                    expand(v & 31, 31),
                    255,
                ],
                _ => [
                    expand(v >> 11, 31),
                    expand((v >> 6) & 31, 31),
                    expand((v >> 1) & 31, 31),
                    if v & 1 != 0 { 255 } else { 0 },
                ],
            };
            out.copy_from_slice(&rgba);
        }
        0x81 | 0x8b => out.copy_from_slice(source),
        _ => {
            let width = if matches!(format, 0x9b | 0x9c) { 4 } else { 2 };
            for (src, dst) in source.chunks_exact(width).zip(out.chunks_exact_mut(width)) {
                for (d, s) in dst.iter_mut().zip(src.iter().rev()) {
                    *d = *s;
                }
            }
        }
    }
}
/// Preserve compressed block bits for native BC upload, rather than lossy CPU
/// expansion. Other formats become tightly packed native component byte order.
pub fn decode_texture_image(
    layout: TextureLayout,
    source: Vec<u8>,
) -> Result<OwnedTextureImage, TextureError> {
    if source.len() != layout.source_bytes {
        return Err(TextureError::Extent);
    }
    let mut levels = Vec::new();
    reserve(&mut levels, layout.levels.len())?;
    for lv in &layout.levels {
        let mut bytes = Vec::new();
        reserve(&mut bytes, lv.output_bytes)?;
        bytes.resize(lv.output_bytes, 0);
        if layout.block_bytes != 0 {
            bytes.copy_from_slice(&source[lv.source_offset..lv.source_offset + lv.output_bytes]);
        } else {
            for y in 0..lv.height {
                for x in 0..lv.width {
                    let at = lv.source_offset
                        + if layout.swizzled {
                            morton(x, y, lv.width.trailing_zeros(), lv.height.trailing_zeros())
                                * layout.source_bpp
                        } else {
                            y as usize * lv.source_row_bytes + x as usize * layout.source_bpp
                        };
                    let to = (y as usize * lv.width as usize + x as usize) * layout.output_bpp;
                    texel(
                        layout.source_format,
                        &source[at..at + layout.source_bpp],
                        &mut bytes[to..to + layout.output_bpp],
                    );
                }
            }
        }
        levels.push(TextureImageLevel {
            face: lv.face,
            level: lv.level,
            width: lv.width,
            height: lv.height,
            bytes,
        });
    }
    Ok(OwnedTextureImage {
        format: layout.format,
        descriptor: layout.descriptor,
        source_bytes: source,
        levels,
    })
}
