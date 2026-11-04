//! All declarations in one owned primary source format, before native binding.
//! Source semantic IDs are preserved; no UV/normal or shader-location guess.
use crate::{
    decoded_geometry_cache::{self, RowCodec},
    geometry_streams::GeometryStreamError,
};
use std::sync::Arc;

#[derive(Clone, Debug)]
pub struct DecodedPrimaryAttribute {
    pub source_declaration: usize,
    pub semantic: u8,
    pub codec: u8,
    pub component_metadata: u8,
    pub words: Arc<Vec<[u32; 4]>>,
}
#[derive(Clone, Debug, PartialEq, Eq)]
pub struct PrimaryAttributeFailure {
    pub source_declaration: usize,
    pub semantic: u8,
    pub error: GeometryStreamError,
}
#[derive(Clone, Debug, PartialEq, Eq)]
pub struct PrimaryAttributeAlias {
    pub source_declaration: usize,
    pub first_declaration: usize,
    pub semantic: u8,
}
#[derive(Clone, Debug)]
pub struct OwnedPrimaryAttributes {
    pub declaration_count: usize,
    pub attributes: Vec<DecodedPrimaryAttribute>,
    pub failures: Vec<PrimaryAttributeFailure>,
    pub aliases: Vec<PrimaryAttributeAlias>,
    bytes: usize,
}
impl OwnedPrimaryAttributes {
    pub fn owned_bytes(&self) -> usize {
        self.bytes
    }
    /// A failed earlier declaration cannot be replaced by a later successful
    /// duplicate. Callers choosing a first-lookup consumer must keep that gap.
    pub fn first_semantic(
        &self,
        semantic: u8,
    ) -> Option<Result<&DecodedPrimaryAttribute, &PrimaryAttributeFailure>> {
        let attribute = self
            .attributes
            .iter()
            .filter(|a| a.semantic == semantic)
            .min_by_key(|a| a.source_declaration);
        let failure = self
            .failures
            .iter()
            .filter(|a| a.semantic == semantic)
            .min_by_key(|a| a.source_declaration);
        match (attribute, failure) {
            (Some(a), Some(f)) => Some(if a.source_declaration < f.source_declaration {
                Ok(a)
            } else {
                Err(f)
            }),
            (Some(a), None) => Some(Ok(a)),
            (None, Some(f)) => Some(Err(f)),
            (None, None) => None,
        }
    }
}
/// The budget bounds retained logical four-lane output bytes. Source format
/// metadata is already owned and its declaration population is byte-bounded.
/// Unsupported/failed declarations remain explicit; they never shorten the
/// intended draw population or make this a complete material/geometry proof.
pub fn decode_primary_attributes(
    format: &[u8],
    source: &[u8],
    source_address: u32,
    count: usize,
    byte_budget: usize,
) -> Result<OwnedPrimaryAttributes, GeometryStreamError> {
    let interned = decoded_geometry_cache::intern_pair(format, source);
    decode_primary_attributes_interned(
        format,
        source,
        interned.as_ref(),
        source_address,
        count,
        byte_budget,
    )
}
/// The same decode, sharing one `intern_pair(format, source)` across every
/// declaration (and with the caller's position decode of the same pair).
pub fn decode_primary_attributes_interned(
    format: &[u8],
    source: &[u8],
    interned: Option<&decoded_geometry_cache::InternedPair<'_>>,
    source_address: u32,
    count: usize,
    byte_budget: usize,
) -> Result<OwnedPrimaryAttributes, GeometryStreamError> {
    let _timer =
        crate::source_acquisition::section(crate::source_acquisition::Section::PrimaryAttributes);
    let header = format.get(..8).ok_or(GeometryStreamError::SourceExtent)?;
    let declarations = format
        .get(8..8 + usize::from(header[0]) * 8)
        .ok_or(GeometryStreamError::SourceExtent)?;
    let mut result = OwnedPrimaryAttributes {
        declaration_count: usize::from(header[0]),
        attributes: Vec::new(),
        failures: Vec::new(),
        aliases: Vec::new(),
        bytes: 0,
    };
    result
        .attributes
        .try_reserve_exact(result.declaration_count)
        .map_err(|_| GeometryStreamError::AllocationFailed)?;
    result
        .failures
        .try_reserve_exact(result.declaration_count)
        .map_err(|_| GeometryStreamError::AllocationFailed)?;
    result
        .aliases
        .try_reserve_exact(result.declaration_count)
        .map_err(|_| GeometryStreamError::AllocationFailed)?;
    let mut first = [None; 256];
    for (ordinal, declaration) in declarations.as_chunks::<8>().0.iter().enumerate() {
        let semantic = declaration[3];
        if let Some(first_declaration) = first[usize::from(semantic)] {
            result.aliases.push(PrimaryAttributeAlias {
                source_declaration: ordinal,
                first_declaration,
                semantic,
            });
        }
        if first[usize::from(semantic)].is_none() {
            first[usize::from(semantic)] = Some(ordinal);
        }
        let decoded = if semantic == 0 {
            Err(GeometryStreamError::UnclassifiedSemantic(semantic))
        } else {
            decoded_geometry_cache::rows_interned(
                RowCodec::Descriptor {
                    ordinal,
                    phase: (source_address & 15) as u8,
                },
                interned,
                format,
                source,
                count,
                byte_budget - result.bytes,
            )
        };
        match decoded {
            Ok(words) => {
                result.bytes += words.len() * 16;
                result.attributes.push(DecodedPrimaryAttribute {
                    source_declaration: ordinal,
                    semantic,
                    codec: declaration[1],
                    component_metadata: declaration[2],
                    words,
                });
            }
            Err(error) => result.failures.push(PrimaryAttributeFailure {
                source_declaration: ordinal,
                semantic,
                error,
            }),
        }
    }
    Ok(result)
}
