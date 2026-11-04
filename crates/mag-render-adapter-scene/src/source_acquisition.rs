//! Reuse around the Main source capture, and its counters.
//!
//! Nothing here skips a guest read across updates. No writer, loader or free
//! signal is observed by this adapter, so a location, a header or a sampled
//! fingerprint can never prove that payload bytes are unchanged. Every reuse
//! across updates is authorised by exact equality of all current bytes, read
//! under the caller's actual source window; anything else falls back to the
//! existing counted copy and decode.
//!
//! Output-identical reuse is on by default (`=0` disables it):
//! * `PS3_RENDER_RESIDENT_TEXTURES`: decoded texture images are retained
//!   across updates and reused only when the freshly read span is identical.
//! * `PS3_RENDER_CONTENT_PROBE`: the shared content catalog looks up a cheap
//!   sampled key first; exact equality alone authorises the returned content.
//! * `PS3_RENDER_RESIDENT_SOURCES=0` disables both at once.
//!
//! Opt-in (`=1`):
//! * `PS3_RENDER_CAPTURE_READ_SHARE`: inside one synchronous capture call a
//!   repeated identical request is served from that call's earlier copy. It
//!   removes callbacks but measured CPU-neutral.
//! * `PS3_RENDER_ACQUISITION_PROFILE`: section timings on the report line.
use crate::material_textures::OwnedTextureImage;
use std::{
    cell::Cell,
    collections::HashMap,
    sync::{Arc, Mutex, OnceLock},
};

fn set(name: &str) -> bool {
    std::env::var_os(name).is_some_and(|v| v == "1")
}
fn cleared(name: &str) -> bool {
    std::env::var_os(name).is_some_and(|v| v == "0")
}
/// Default-on switch: off only when it or the umbrella is explicitly `0`.
fn flag(name: &str) -> bool {
    !cleared(name) && !cleared("PS3_RENDER_RESIDENT_SOURCES")
}
/// Default-off `PS3_RENDER_CAPTURE_CONCURRENT_CACHES=1`: the shared caches the
/// capture uses from several threads (content catalog, decoded geometry
/// cache, store generation check) hold their locks only for lookups and
/// inserts; hashing, byte comparison and decoding run outside them, and the
/// decoded cache waits for its lock instead of skipping itself. Results are
/// identical; only lock hold times change.
pub fn concurrent_caches_enabled() -> bool {
    static F: OnceLock<bool> = OnceLock::new();
    *F.get_or_init(|| set("PS3_RENDER_CAPTURE_CONCURRENT_CACHES"))
}
/// Default-off `PS3_RENDER_CAPTURE_VIEW_VALIDATION=1`: a located texture span
/// with a retained image is compared in place, through a read-only view of
/// the bridge's capture window, instead of being copied and then compared.
/// Equal bytes reuse the retained image exactly as the copy path does; any
/// other outcome falls back to the ordinary copy. Requires the bridge to
/// register its view (same flag) and resident textures.
pub fn view_validation_enabled() -> bool {
    static F: OnceLock<bool> = OnceLock::new();
    *F.get_or_init(|| set("PS3_RENDER_CAPTURE_VIEW_VALIDATION"))
}
pub fn read_share_enabled() -> bool {
    static F: OnceLock<bool> = OnceLock::new();
    *F.get_or_init(|| set("PS3_RENDER_CAPTURE_READ_SHARE"))
}
pub fn resident_textures_enabled() -> bool {
    static F: OnceLock<bool> = OnceLock::new();
    *F.get_or_init(|| flag("PS3_RENDER_RESIDENT_TEXTURES"))
}
pub fn content_probe_enabled() -> bool {
    static F: OnceLock<bool> = OnceLock::new();
    *F.get_or_init(|| flag("PS3_RENDER_CONTENT_PROBE"))
}
/// Whether the rate-limited report line is printed: only on explicit request,
/// so default runs keep their console output.
pub fn any_enabled() -> bool {
    static F: OnceLock<bool> = OnceLock::new();
    *F.get_or_init(|| {
        read_share_enabled()
            || profile_enabled()
            || [
                "PS3_RENDER_RESIDENT_TEXTURES",
                "PS3_RENDER_CONTENT_PROBE",
                "PS3_RENDER_RESIDENT_SOURCES",
            ]
            .iter()
            .any(|n| std::env::var_os(n).is_some())
    })
}

/// Per-capture acquisition counters. The existing per-phase byte/read counters
/// keep meaning logical source requests; these say how many of those reached
/// the guest callback and how many were satisfied by reuse.
#[derive(Clone, Copy, Debug, Default, PartialEq, Eq)]
pub struct AcquisitionStats {
    /// Source callbacks actually performed (each one a complete guest copy).
    pub guest_callbacks: u64,
    pub guest_callback_bytes: u64,
    /// Requests served from an identical earlier request in the same capture.
    pub shared_reads: u64,
    pub shared_bytes: u64,
    /// Cross-update resident reuse, authorised by exact byte equality.
    pub resident_hits: u64,
    pub resident_hit_bytes: u64,
    /// No retained entry for this key.
    pub resident_misses: u64,
    /// A retained entry existed but the current bytes differ: replaced.
    pub resident_invalidations: u64,
    /// Read buffers served from recycled validation buffers.
    pub pooled_buffers: u64,
    /// Spans compared in place through the capture view (copy-free path):
    /// equal (reused without a copy) and differing (then copied as usual).
    pub view_equal: u64,
    pub view_equal_bytes: u64,
    pub view_differ: u64,
}
impl AcquisitionStats {
    pub fn plus(self, other: Self) -> Self {
        Self {
            guest_callbacks: self.guest_callbacks + other.guest_callbacks,
            guest_callback_bytes: self.guest_callback_bytes + other.guest_callback_bytes,
            shared_reads: self.shared_reads + other.shared_reads,
            shared_bytes: self.shared_bytes + other.shared_bytes,
            resident_hits: self.resident_hits + other.resident_hits,
            resident_hit_bytes: self.resident_hit_bytes + other.resident_hit_bytes,
            resident_misses: self.resident_misses + other.resident_misses,
            resident_invalidations: self.resident_invalidations + other.resident_invalidations,
            pooled_buffers: self.pooled_buffers + other.pooled_buffers,
            view_equal: self.view_equal + other.view_equal,
            view_equal_bytes: self.view_equal_bytes + other.view_equal_bytes,
            view_differ: self.view_differ + other.view_differ,
        }
    }
    pub fn since(self, earlier: Self) -> Self {
        Self {
            guest_callbacks: self.guest_callbacks.wrapping_sub(earlier.guest_callbacks),
            guest_callback_bytes: self
                .guest_callback_bytes
                .wrapping_sub(earlier.guest_callback_bytes),
            shared_reads: self.shared_reads.wrapping_sub(earlier.shared_reads),
            shared_bytes: self.shared_bytes.wrapping_sub(earlier.shared_bytes),
            resident_hits: self.resident_hits.wrapping_sub(earlier.resident_hits),
            resident_hit_bytes: self
                .resident_hit_bytes
                .wrapping_sub(earlier.resident_hit_bytes),
            resident_misses: self.resident_misses.wrapping_sub(earlier.resident_misses),
            resident_invalidations: self
                .resident_invalidations
                .wrapping_sub(earlier.resident_invalidations),
            pooled_buffers: self.pooled_buffers.wrapping_sub(earlier.pooled_buffers),
            view_equal: self.view_equal.wrapping_sub(earlier.view_equal),
            view_equal_bytes: self.view_equal_bytes.wrapping_sub(earlier.view_equal_bytes),
            view_differ: self.view_differ.wrapping_sub(earlier.view_differ),
        }
    }
}
thread_local! {
    // Capture is synchronous on the calling thread, so a thread-local running
    // total gives exact per-capture deltas without cross-thread attribution.
    static STATS: Cell<AcquisitionStats> = const { Cell::new(AcquisitionStats {
        guest_callbacks: 0,
        guest_callback_bytes: 0,
        shared_reads: 0,
        shared_bytes: 0,
        resident_hits: 0,
        resident_hit_bytes: 0,
        resident_misses: 0,
        resident_invalidations: 0,
        pooled_buffers: 0,
        view_equal: 0,
        view_equal_bytes: 0,
        view_differ: 0,
    }) };
}
pub fn thread_stats() -> AcquisitionStats {
    STATS.with(Cell::get)
}
fn count(update: impl FnOnce(&mut AcquisitionStats)) {
    STATS.with(|s| {
        let mut v = s.get();
        update(&mut v);
        s.set(v);
    });
}
pub(crate) fn count_view(equal: bool, bytes: usize) {
    count(|s| {
        if equal {
            s.view_equal += 1;
            s.view_equal_bytes += bytes as u64;
        } else {
            s.view_differ += 1;
        }
    });
}
pub(crate) fn count_callback(bytes: usize) {
    count(|s| {
        s.guest_callbacks += 1;
        s.guest_callback_bytes += bytes as u64;
    });
}

/// Opt-in inclusive wall-time sections (`PS3_RENDER_ACQUISITION_PROFILE=1`).
/// Sections nest (for example callbacks inside texture acquisition), so they
/// are attribution aids and do not add up to the phase wall times.
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
#[repr(usize)]
pub enum Section {
    GuestCallback,
    DecodedCacheLookup,
    PrimaryAttributes,
    AuxiliaryAttributes,
    GroupZeroPreparation,
    NoninstancedPreparation,
    SkinningPreparation,
    StaticPreparation,
    TextureImages,
    MaterialIntern,
}
pub const SECTIONS: usize = 10;
pub fn profile_enabled() -> bool {
    static F: OnceLock<bool> = OnceLock::new();
    *F.get_or_init(|| set("PS3_RENDER_ACQUISITION_PROFILE"))
}
thread_local! {
    static PROFILE: Cell<[u64; SECTIONS]> = const { Cell::new([0; SECTIONS]) };
}
pub struct SectionTimer(Option<(Section, std::time::Instant)>);
impl Drop for SectionTimer {
    fn drop(&mut self) {
        if let Some((section, start)) = self.0 {
            let ns = u64::try_from(start.elapsed().as_nanos()).unwrap_or(u64::MAX);
            PROFILE.with(|p| {
                let mut v = p.get();
                v[section as usize] = v[section as usize].saturating_add(ns);
                p.set(v);
            });
        }
    }
}
/// A guard that charges its lifetime to `section` when profiling is enabled.
/// Disabled, it takes no clock reading.
pub fn section(section: Section) -> SectionTimer {
    SectionTimer(profile_enabled().then(|| (section, std::time::Instant::now())))
}
pub fn thread_profile() -> [u64; SECTIONS] {
    PROFILE.with(Cell::get)
}

#[derive(Clone, Copy, Debug, PartialEq, Eq, Hash)]
pub enum ReadDomain {
    Guest,
    Local,
    Main,
}
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub struct ShareLimits {
    pub record: usize,
    pub total: usize,
    pub entries: usize,
}
impl Default for ShareLimits {
    fn default() -> Self {
        Self {
            record: 1024 * 1024,
            total: 32 * 1024 * 1024,
            entries: 65536,
        }
    }
}
enum Seen {
    Once,
    Copied(Vec<u8>),
}
/// Request keys are adapter-chosen integers, not attacker-controlled input; a
/// multiply-rotate hash is enough for one bounded per-capture table.
#[derive(Default, Clone, Copy)]
pub struct RequestHasher(u64);
impl std::hash::Hasher for RequestHasher {
    fn finish(&self) -> u64 {
        self.0
    }
    fn write(&mut self, bytes: &[u8]) {
        for &b in bytes {
            self.write_u64(u64::from(b));
        }
    }
    fn write_u8(&mut self, v: u8) {
        self.write_u64(u64::from(v));
    }
    fn write_u32(&mut self, v: u32) {
        self.write_u64(u64::from(v));
    }
    fn write_usize(&mut self, v: usize) {
        self.write_u64(v as u64);
    }
    fn write_isize(&mut self, v: isize) {
        self.write_u64(v as u64);
    }
    fn write_u64(&mut self, v: u64) {
        self.0 = (self.0.rotate_left(5) ^ v).wrapping_mul(0x517c_c1b7_2722_0a95);
    }
}
type RequestMap<V> =
    HashMap<(ReadDomain, u32, usize), V, std::hash::BuildHasherDefault<RequestHasher>>;
/// Identical requests inside ONE synchronous capture call. Its lifetime is that
/// call: it is never retained across captures, and it is not an asset version.
/// The first request of a range costs only a key; a range is copied for reuse
/// when it is requested a second time, so single-use ranges add no copy.
pub struct CaptureReadShare {
    enabled: bool,
    limits: ShareLimits,
    seen: RequestMap<Seen>,
    retained: usize,
}
impl CaptureReadShare {
    pub fn new(enabled: bool, limits: ShareLimits) -> Self {
        Self {
            enabled,
            limits,
            seen: RequestMap::default(),
            retained: 0,
        }
    }
    /// An owned copy of an identical earlier request in this capture, if any.
    pub fn shared(&mut self, domain: ReadDomain, address: u32, bytes: usize) -> Option<Vec<u8>> {
        if !self.enabled {
            return None;
        }
        let Some(Seen::Copied(copy)) = self.seen.get(&(domain, address, bytes)) else {
            return None;
        };
        let mut out = Vec::new();
        out.try_reserve_exact(copy.len()).ok()?;
        out.extend_from_slice(copy);
        count(|s| {
            s.shared_reads += 1;
            s.shared_bytes += bytes as u64;
        });
        Some(out)
    }
    /// Note a completed callback; copy it for reuse on its second occurrence.
    pub fn record(&mut self, domain: ReadDomain, address: u32, bytes: &[u8]) {
        if !self.enabled || bytes.len() > self.limits.record {
            return;
        }
        let key = (domain, address, bytes.len());
        match self.seen.get_mut(&key) {
            None => {
                if self.seen.len() < self.limits.entries {
                    self.seen.insert(key, Seen::Once);
                }
            }
            Some(seen @ Seen::Once) => {
                if self.retained + bytes.len() <= self.limits.total {
                    let mut copy = Vec::new();
                    if copy.try_reserve_exact(bytes.len()).is_ok() {
                        copy.extend_from_slice(bytes);
                        self.retained += bytes.len();
                        *seen = Seen::Copied(copy);
                    }
                }
            }
            Some(Seen::Copied(_)) => {}
        }
    }
}

/// Recycled read buffers. Only buffers proven dead (a validated-equal resident
/// read) come back. Every byte of a pooled buffer is initialized; the callback
/// overwrites the requested extent before any byte is used.
struct Pool {
    buffers: Vec<Vec<u8>>,
    bytes: usize,
}
const POOL_MIN: usize = 64 * 1024;
const POOL_BYTES: usize = 64 * 1024 * 1024;
const POOL_BUFFERS: usize = 8;
static POOL: Mutex<Pool> = Mutex::new(Pool {
    buffers: Vec::new(),
    bytes: 0,
});
/// A buffer of exactly `bytes` initialized bytes, recycled when available.
pub fn read_buffer(bytes: usize, pooled: bool) -> Option<Vec<u8>> {
    if pooled
        && bytes >= POOL_MIN
        && let Ok(mut pool) = POOL.lock()
        && let Some(index) = pool
            .buffers
            .iter()
            .enumerate()
            .filter(|(_, b)| b.capacity() >= bytes)
            .min_by_key(|(_, b)| b.capacity())
            .map(|(i, _)| i)
    {
        let mut buffer = pool.buffers.swap_remove(index);
        pool.bytes -= buffer.capacity();
        drop(pool);
        if buffer.len() >= bytes {
            buffer.truncate(bytes);
        } else {
            buffer.resize(bytes, 0);
        }
        count(|s| s.pooled_buffers += 1);
        return Some(buffer);
    }
    let mut owned = Vec::new();
    owned.try_reserve_exact(bytes).ok()?;
    owned.resize(bytes, 0);
    Some(owned)
}
/// A recycled, fully initialized buffer of exactly `bytes`, if one is pooled.
pub fn pooled_buffer(bytes: usize, pooled: bool) -> Option<Vec<u8>> {
    if !pooled || bytes < POOL_MIN {
        return None;
    }
    let buffer = read_buffer(bytes, true)?;
    // read_buffer falls back to a fresh zeroed allocation; that is still a
    // valid initialized buffer of this length.
    Some(buffer)
}
pub fn recycle(buffer: Vec<u8>) {
    if buffer.capacity() < POOL_MIN {
        return;
    }
    if let Ok(mut pool) = POOL.lock() {
        while !pool.buffers.is_empty()
            && (pool.buffers.len() >= POOL_BUFFERS || pool.bytes + buffer.capacity() > POOL_BYTES)
        {
            // Keep the largest buffers: they can serve every smaller span.
            let smallest = pool
                .buffers
                .iter()
                .enumerate()
                .min_by_key(|(_, b)| b.capacity())
                .map(|(i, _)| i)
                .expect("non-empty pool");
            if pool.buffers[smallest].capacity() >= buffer.capacity() {
                return;
            }
            let old = pool.buffers.swap_remove(smallest);
            pool.bytes -= old.capacity();
        }
        if pool.bytes + buffer.capacity() <= POOL_BYTES {
            pool.bytes += buffer.capacity();
            pool.buffers.push(buffer);
        }
    }
}

#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub struct ResidentLimits {
    pub total: usize,
    pub entries: usize,
}
impl Default for ResidentLimits {
    fn default() -> Self {
        Self {
            total: 192 * 1024 * 1024,
            entries: 4096,
        }
    }
}
struct ResidentTexture {
    image: Arc<OwnedTextureImage>,
    bytes: usize,
    touched: u64,
}
/// Decoded texture images kept across updates. The key is the complete source
/// descriptor (format, extent, levels, location, offset and controls), which
/// fully determines the decode layout; it selects a candidate only. Reuse also
/// requires every currently read source byte to equal the retained span.
/// Entries are shared immutable `Arc`s: replacing or evicting one never
/// revokes a version an earlier capture, packet or GPU upload still holds.
#[derive(Default)]
pub struct ResidentTextures {
    entries: HashMap<[u32; 8], ResidentTexture>,
    bytes: usize,
    clock: u64,
}
fn image_bytes(image: &OwnedTextureImage) -> usize {
    image.levels.iter().fold(image.source_bytes.len(), |n, l| {
        n.saturating_add(l.bytes.len())
    })
}
impl ResidentTextures {
    /// The retained image if `current` is byte-identical to its source span.
    /// A differing entry is removed and counted as an invalidation.
    pub fn reuse(
        &mut self,
        descriptor: [u32; 8],
        current: &[u8],
    ) -> Option<Arc<OwnedTextureImage>> {
        self.clock += 1;
        let touched = self.clock;
        let Some(entry) = self.entries.get_mut(&descriptor) else {
            count(|s| s.resident_misses += 1);
            return None;
        };
        if entry.image.descriptor == descriptor && entry.image.source_bytes == current {
            entry.touched = touched;
            count(|s| {
                s.resident_hits += 1;
                s.resident_hit_bytes += current.len() as u64;
            });
            return Some(Arc::clone(&entry.image));
        }
        let old = self.entries.remove(&descriptor).expect("present entry");
        self.bytes -= old.bytes;
        count(|s| s.resident_invalidations += 1);
        None
    }
    /// The retained image for `descriptor`, without touching or counting it.
    pub fn candidate(&self, descriptor: [u32; 8]) -> Option<Arc<OwnedTextureImage>> {
        self.entries
            .get(&descriptor)
            .filter(|e| e.image.descriptor == descriptor)
            .map(|e| Arc::clone(&e.image))
    }
    /// Record a reuse authorised by an in-place comparison with `image`'s
    /// source bytes: the same touch and hit counters as `reuse`.
    pub fn confirm(&mut self, descriptor: [u32; 8], image: &Arc<OwnedTextureImage>) {
        self.clock += 1;
        let touched = self.clock;
        if let Some(entry) = self.entries.get_mut(&descriptor)
            && Arc::ptr_eq(&entry.image, image)
        {
            entry.touched = touched;
        }
        count(|s| {
            s.resident_hits += 1;
            s.resident_hit_bytes += image.source_bytes.len() as u64;
        });
    }
    pub fn remember(&mut self, image: &Arc<OwnedTextureImage>, limits: ResidentLimits) {
        let bytes = image_bytes(image);
        if bytes > limits.total || limits.entries == 0 {
            return;
        }
        if let Some(old) = self.entries.remove(&image.descriptor) {
            self.bytes -= old.bytes;
        }
        while !self.entries.is_empty()
            && (self.entries.len() >= limits.entries || self.bytes + bytes > limits.total)
        {
            let victim = *self
                .entries
                .iter()
                .min_by_key(|(_, e)| e.touched)
                .map(|(k, _)| k)
                .expect("non-empty");
            let old = self.entries.remove(&victim).expect("victim");
            self.bytes -= old.bytes;
        }
        self.clock += 1;
        self.entries.insert(
            image.descriptor,
            ResidentTexture {
                image: Arc::clone(image),
                bytes,
                touched: self.clock,
            },
        );
        self.bytes += bytes;
    }
    pub fn clear(&mut self) {
        self.entries.clear();
        self.bytes = 0;
    }
    pub fn len(&self) -> usize {
        self.entries.len()
    }
    pub fn is_empty(&self) -> bool {
        self.entries.is_empty()
    }
    pub fn retained_bytes(&self) -> usize {
        self.bytes
    }
}
static TEXTURES: OnceLock<Mutex<ResidentTextures>> = OnceLock::new();
fn textures() -> &'static Mutex<ResidentTextures> {
    TEXTURES.get_or_init(|| Mutex::new(ResidentTextures::default()))
}
/// Live capture hook: `Ok` returns the retained identical image and recycles
/// the validation buffer; `Err` hands the bytes back for the ordinary decode.
pub(crate) fn reuse_texture(
    descriptor: [u32; 8],
    current: Vec<u8>,
) -> Result<Arc<OwnedTextureImage>, Vec<u8>> {
    if !resident_textures_enabled() {
        return Err(current);
    }
    let found = textures()
        .lock()
        .ok()
        .and_then(|mut t| t.reuse(descriptor, &current));
    match found {
        Some(image) => {
            recycle(current);
            Ok(image)
        }
        None => {
            // A recycled buffer may be larger than this span; do not let a new
            // retained image keep that slack alive.
            let mut current = current;
            if current.capacity() > current.len() + current.len() / 4 {
                current.shrink_to_fit();
            }
            Err(current)
        }
    }
}
pub(crate) fn resident_candidate(descriptor: [u32; 8]) -> Option<Arc<OwnedTextureImage>> {
    if !resident_textures_enabled() {
        return None;
    }
    textures().lock().ok()?.candidate(descriptor)
}
pub(crate) fn confirm_resident(descriptor: [u32; 8], image: &Arc<OwnedTextureImage>) {
    if let Ok(mut t) = textures().lock() {
        t.confirm(descriptor, image);
    }
}
pub(crate) fn remember_texture(image: &Arc<OwnedTextureImage>) {
    if resident_textures_enabled()
        && let Ok(mut t) = textures().lock()
    {
        t.remember(image, ResidentLimits::default());
    }
}
/// (entries, retained bytes) of the live resident texture store.
pub fn resident_texture_occupancy() -> (usize, usize) {
    TEXTURES
        .get()
        .and_then(|t| t.try_lock().ok())
        .map_or((0, 0), |t| (t.len(), t.retained_bytes()))
}
/// A different VM has no relation to retained bytes; drop every entry.
pub(crate) fn vm_changed() {
    if let Some(t) = TEXTURES.get()
        && let Ok(mut t) = t.lock()
    {
        t.clear();
    }
    if let Ok(mut p) = POOL.lock() {
        p.buffers.clear();
        p.bytes = 0;
    }
}

#[cfg(test)]
mod tests {
    use super::*;
    use crate::material_textures::{decode_texture_image, texture_layout};

    fn descriptor(offset: u32, format: u8) -> [u32; 8] {
        [
            offset,
            2 | 0x20 | (u32::from(format) << 8) | (1 << 16),
            0,
            0x8000_0000,
            0xaae4,
            0,
            (4 << 16) | 4,
            0,
        ]
    }
    fn image(d: [u32; 8], fill: u8) -> Arc<OwnedTextureImage> {
        let layout = texture_layout(d, usize::MAX, usize::MAX).unwrap();
        let bytes = vec![fill; layout.source_bytes()];
        Arc::new(decode_texture_image(layout, bytes).unwrap())
    }

    #[test]
    fn resident_texture_reuses_only_exact_bytes_and_counts_each_outcome() {
        let mut store = ResidentTextures::default();
        let limits = ResidentLimits::default();
        let d = descriptor(0x1000, 0x86);
        let first = image(d, 7);
        let before = thread_stats();
        assert!(store.reuse(d, &first.source_bytes).is_none()); // miss
        store.remember(&first, limits);
        let hit = store.reuse(d, &first.source_bytes.clone()).unwrap();
        assert!(Arc::ptr_eq(&hit, &first));
        // One changed byte anywhere (here the last) is a mutation.
        let mut mutated = first.source_bytes.clone();
        *mutated.last_mut().unwrap() ^= 1;
        assert!(store.reuse(d, &mutated).is_none());
        assert!(store.is_empty()); // the stale version is gone
        assert!(store.reuse(d, &first.source_bytes).is_none()); // now a miss
        let stats = thread_stats().since(before);
        assert_eq!(
            (
                stats.resident_misses,
                stats.resident_hits,
                stats.resident_invalidations,
                stats.resident_hit_bytes
            ),
            (2, 1, 1, first.source_bytes.len() as u64)
        );
    }

    #[test]
    fn same_bytes_under_another_descriptor_or_location_is_not_reused() {
        let mut store = ResidentTextures::default();
        let a = image(descriptor(0x1000, 0x86), 3);
        store.remember(&a, ResidentLimits::default());
        // Same location/bytes, different format or a different offset: a key
        // is never the location alone.
        assert!(
            store
                .reuse(descriptor(0x1000, 0x88), &a.source_bytes)
                .is_none()
        );
        assert!(
            store
                .reuse(descriptor(0x2000, 0x86), &a.source_bytes)
                .is_none()
        );
        // Extent mismatch with an equal prefix is also not equality.
        let mut longer = a.source_bytes.clone();
        longer.push(3);
        assert!(store.reuse(a.descriptor, &longer).is_none());
    }

    #[test]
    fn free_and_reload_replace_the_version_while_inflight_holders_keep_theirs() {
        let mut store = ResidentTextures::default();
        let limits = ResidentLimits::default();
        let d = descriptor(0x1000, 0x86);
        let old = image(d, 1);
        store.remember(&old, limits);
        let inflight = store.reuse(d, &old.source_bytes).unwrap();
        // Freed (zeroed) memory differs: no stale reuse.
        assert!(store.reuse(d, &vec![0; old.source_bytes.len()]).is_none());
        // A different asset reloaded at the same location with same descriptor.
        let new = image(d, 9);
        assert!(store.reuse(d, &new.source_bytes).is_none());
        store.remember(&new, limits);
        assert!(Arc::ptr_eq(
            &store.reuse(d, &new.source_bytes).unwrap(),
            &new
        ));
        // The earlier capture's version is intact until its last holder drops.
        assert!(inflight.source_bytes.iter().all(|&b| b == 1));
        assert!(inflight.levels[0].bytes.iter().all(|&b| b == 1));
        drop(old);
        assert_eq!(Arc::strong_count(&inflight), 1);
    }

    #[test]
    fn resident_store_is_bounded_and_evicts_least_recent() {
        let mut store = ResidentTextures::default();
        let a = image(descriptor(0x1000, 0x86), 1);
        let b = image(descriptor(0x2000, 0x86), 2);
        let c = image(descriptor(0x3000, 0x86), 3);
        let limits = ResidentLimits {
            total: usize::MAX,
            entries: 2,
        };
        store.remember(&a, limits);
        store.remember(&b, limits);
        assert!(store.reuse(a.descriptor, &a.source_bytes).is_some()); // a recent
        store.remember(&c, limits);
        assert_eq!(store.len(), 2);
        assert!(store.reuse(b.descriptor, &b.source_bytes).is_none());
        assert!(store.reuse(a.descriptor, &a.source_bytes).is_some());
        let tight = ResidentLimits {
            total: image_bytes(&a) - 1,
            entries: 8,
        };
        let mut small = ResidentTextures::default();
        small.remember(&a, tight);
        assert!(small.is_empty()); // too large for the budget: not retained
        store.clear();
        assert_eq!((store.len(), store.retained_bytes()), (0, 0));
    }

    #[test]
    fn capture_share_copies_on_second_request_and_never_when_disabled() {
        let before = thread_stats();
        let mut off = CaptureReadShare::new(false, ShareLimits::default());
        off.record(ReadDomain::Guest, 0x100, &[1, 2, 3]);
        off.record(ReadDomain::Guest, 0x100, &[1, 2, 3]);
        assert!(off.shared(ReadDomain::Guest, 0x100, 3).is_none());
        let mut on = CaptureReadShare::new(true, ShareLimits::default());
        on.record(ReadDomain::Guest, 0x100, &[1, 2, 3]);
        assert!(on.shared(ReadDomain::Guest, 0x100, 3).is_none()); // seen once
        on.record(ReadDomain::Guest, 0x100, &[1, 2, 3]);
        assert_eq!(on.shared(ReadDomain::Guest, 0x100, 3).unwrap(), [1, 2, 3]);
        // Length and domain are part of the request identity.
        assert!(on.shared(ReadDomain::Guest, 0x100, 2).is_none());
        assert!(on.shared(ReadDomain::Local, 0x100, 3).is_none());
        let stats = thread_stats().since(before);
        assert_eq!((stats.shared_reads, stats.shared_bytes), (1, 3));
        let mut tiny = CaptureReadShare::new(
            true,
            ShareLimits {
                record: 2,
                total: 4,
                entries: 1,
            },
        );
        tiny.record(ReadDomain::Guest, 0x100, &[1, 2, 3]);
        tiny.record(ReadDomain::Guest, 0x100, &[1, 2, 3]);
        assert!(tiny.shared(ReadDomain::Guest, 0x100, 3).is_none()); // over record
        tiny.record(ReadDomain::Guest, 0x200, &[1]);
        tiny.record(ReadDomain::Guest, 0x300, &[1]);
        tiny.record(ReadDomain::Guest, 0x300, &[1]);
        assert!(tiny.shared(ReadDomain::Guest, 0x300, 1).is_none()); // entry cap
    }

    #[test]
    fn disabled_profile_takes_no_readings() {
        if profile_enabled() {
            return; // only meaningful when this process leaves the switch off
        }
        let before = thread_profile();
        drop(section(Section::GuestCallback));
        assert_eq!(thread_profile(), before);
    }

    #[test]
    fn pooled_buffers_have_exact_initialized_extent() {
        let size = POOL_MIN + 12345;
        let mut first = read_buffer(size, true).unwrap();
        assert_eq!(first.len(), size);
        first.fill(0xaa);
        recycle(first);
        let before = thread_stats();
        let smaller = read_buffer(size - 100, true).unwrap();
        assert_eq!(smaller.len(), size - 100);
        let reused = thread_stats().since(before).pooled_buffers;
        recycle(smaller);
        let larger = read_buffer(size + 1, true).unwrap();
        assert_eq!(larger.len(), size + 1);
        // Small or unpooled requests are fresh zeroed allocations.
        assert_eq!(read_buffer(16, true).unwrap(), vec![0; 16]);
        assert!(read_buffer(size, false).unwrap().iter().all(|&b| b == 0));
        // Another parallel test can take a pooled buffer first; never more.
        assert!(reused <= 1);
    }
}
