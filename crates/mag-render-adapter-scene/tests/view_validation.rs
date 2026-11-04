//! Opt-in copy-free validation of retained textures
//! (`PS3_RENDER_CAPTURE_VIEW_VALIDATION=1`), with resident textures on. The
//! copy path is the reference: same images, same reuse decisions, same
//! accounted reads, and only the copies differ.
use mag_render_adapter_scene::{
    capture_view::mag_render_adapter_set_located_view,
    error::Error,
    material_textures::{
        LocatedSource, LocatedTextureRequest, OwnedTextureImage, ReadOnly, TextureError,
        TextureImageCache, copy_texture_source_image_from,
    },
    source_acquisition,
};
use std::sync::{Arc, Mutex};

static LOCK: Mutex<()> = Mutex::new(());
fn enable() -> std::sync::MutexGuard<'static, ()> {
    // SAFETY: every test in this process sets the same values under one lock
    // before any adapter call.
    let guard = LOCK.lock().unwrap_or_else(|e| e.into_inner());
    unsafe {
        std::env::set_var("PS3_RENDER_RESIDENT_SOURCES", "1");
        std::env::set_var("PS3_RENDER_CAPTURE_VIEW_VALIDATION", "1");
    }
    assert!(source_acquisition::resident_textures_enabled());
    assert!(source_acquisition::view_validation_enabled());
    guard
}
fn bc1(offset: u32, width: u32, height: u32) -> [u32; 8] {
    [
        offset,
        2 | 0x20 | (0x86 << 8) | (1 << 16),
        0,
        0x8000_0000,
        0xaae4,
        0,
        (width << 16) | height,
        0,
    ]
}
/// A guest region read by copy, or (when `view`) compared in place first,
/// with the material reader's accounting: an in-place match accounts the read
/// exactly as a copy would; a mismatch accounts nothing.
struct Guest<'a> {
    memory: &'a [u8],
    view: bool,
    copies: usize,
    accounted: usize,
}
impl LocatedSource for Guest<'_> {
    fn read(&mut self, r: LocatedTextureRequest) -> Result<Vec<u8>, TextureError> {
        self.accounted += r.bytes;
        self.copies += 1;
        if self.memory.is_empty() {
            return Err(TextureError::Source(Error::OutsideSnapshot));
        }
        Ok(self.memory[r.offset as usize..r.offset as usize + r.bytes].to_vec())
    }
    fn validate(
        &mut self,
        r: LocatedTextureRequest,
        expected: &[u8],
    ) -> Result<bool, TextureError> {
        if !self.view || self.memory.is_empty() {
            return Ok(false);
        }
        let span = &self.memory[r.offset as usize..r.offset as usize + r.bytes];
        if span != expected {
            return Ok(false);
        }
        self.accounted += r.bytes;
        Ok(true)
    }
}
fn update(
    descriptor: [u32; 8],
    memory: &[u8],
    view: bool,
) -> (Result<Arc<OwnedTextureImage>, TextureError>, usize, usize) {
    let mut source = Guest {
        memory,
        view,
        copies: 0,
        accounted: 0,
    };
    let mut remaining = usize::MAX;
    let image = copy_texture_source_image_from(
        descriptor,
        &mut remaining,
        &mut TextureImageCache::default(),
        &mut source,
    );
    (image, source.copies, source.accounted)
}
/// The same update sequence as the copy-path suite: first sight, unchanged,
/// one mutated block, freed range, reload, unchanged again.
fn sequence(
    descriptor: [u32; 8],
    view: bool,
) -> (
    Vec<String>,
    source_acquisition::AcquisitionStats,
    Vec<usize>,
) {
    let span = 256 * 256 / 2;
    let offset = descriptor[0] as usize;
    let mut guest = vec![0u8; offset + span];
    for (i, b) in guest[offset..].iter_mut().enumerate() {
        *b = (i * 13 + 1) as u8;
    }
    let before = source_acquisition::thread_stats();
    let mut out = Vec::new();
    let mut copies = Vec::new();
    let mut previous: Option<Arc<OwnedTextureImage>> = None;
    let mut step = |memory: &[u8], out: &mut Vec<String>| {
        let (image, copied, accounted) = update(descriptor, memory, view);
        copies.push(copied);
        out.push(match &image {
            Ok(i) => format!(
                "ok accounted={accounted} same_as_previous={} levels={:?}",
                previous.as_ref().is_some_and(|p| Arc::ptr_eq(p, i)),
                i.levels.iter().map(|l| l.bytes.clone()).collect::<Vec<_>>()
            ),
            Err(e) => format!("err accounted={accounted} {e:?}"),
        });
        previous = image.ok();
    };
    step(&guest, &mut out);
    step(&guest, &mut out);
    guest[offset + span / 2] ^= 0x55;
    step(&guest, &mut out);
    step(&[], &mut out);
    guest[offset + span / 2] ^= 0x55;
    step(&guest, &mut out);
    step(&guest, &mut out);
    (
        out,
        source_acquisition::thread_stats().since(before),
        copies,
    )
}

#[test]
fn in_place_validation_reuses_exactly_what_the_copy_path_reuses() {
    let _guard = enable();
    // Distinct descriptors: each sequence starts from first sight.
    let (copy, copy_stats, copy_copies) = sequence(bc1(0x1000, 256, 256), false);
    let (view, view_stats, view_copies) = sequence(bc1(0x2000, 256, 256), true);
    assert_eq!(view, copy);
    // Same reuse decisions and resident bookkeeping.
    assert_eq!(view_stats.resident_hits, copy_stats.resident_hits);
    assert_eq!(view_stats.resident_misses, copy_stats.resident_misses);
    assert_eq!(
        view_stats.resident_invalidations,
        copy_stats.resident_invalidations
    );
    assert_eq!(view_stats.resident_hit_bytes, copy_stats.resident_hit_bytes);
    // Only the copies of unchanged spans disappear: first sight, the mutated
    // span, the freed read and the reload are still copied.
    assert_eq!(copy_copies, [1, 1, 1, 1, 1, 1]);
    assert_eq!(view_copies, [1, 0, 1, 1, 1, 0]);
}

#[test]
fn the_plain_read_closure_never_validates() {
    let _guard = enable();
    let descriptor = bc1(0x8000, 256, 256);
    let memory: Vec<u8> = (0..0x8000 + 256 * 256 / 2).map(|i| (i * 7) as u8).collect();
    for _ in 0..2 {
        let mut copies = 0;
        let mut read = |r: LocatedTextureRequest| {
            copies += 1;
            Ok(memory[r.offset as usize..r.offset as usize + r.bytes].to_vec())
        };
        copy_texture_source_image_from(
            descriptor,
            &mut usize::MAX.clone(),
            &mut TextureImageCache::default(),
            &mut ReadOnly(&mut read),
        )
        .unwrap();
        assert_eq!(copies, 1);
    }
}

#[test]
fn located_view_registration_is_once_and_idempotent() {
    unsafe extern "C" fn a(_: *mut core::ffi::c_void, _: u32, _: u32, _: u32) -> *const u8 {
        std::ptr::null()
    }
    unsafe extern "C" fn b(_: *mut core::ffi::c_void, _: u32, _: u32, _: u32) -> *const u8 {
        std::ptr::null()
    }
    // SAFETY: test views never dereference anything.
    unsafe {
        assert_eq!(mag_render_adapter_set_located_view(None), 1);
        assert_eq!(mag_render_adapter_set_located_view(Some(a)), 0);
        assert_eq!(mag_render_adapter_set_located_view(Some(a)), 0);
        assert_eq!(mag_render_adapter_set_located_view(Some(b)), 1);
    }
}

/// Synthetic cost (release, --ignored --nocapture): one warm update of 345
/// retained BC1 spans totalling about 37 MiB, copy path against in-place
/// validation.
#[test]
#[ignore]
fn bench_warm_texture_validation() {
    let _guard = enable();
    let count = 345usize;
    let span = 256 * 256 / 2 * 4; // 512x512 BC1: 128 KiB
    let memory: Vec<u8> = (0..count * span)
        .map(|i| (i * 31 + i / 4099) as u8)
        .collect();
    let descriptors: Vec<[u32; 8]> = (0..count)
        .map(|i| bc1(0x1000_0000 + (i * span) as u32, 512, 512))
        .collect();
    let base = 0x1000_0000usize;
    let run = |view: bool| {
        let start = std::time::Instant::now();
        for d in &descriptors {
            let mut source = Guest {
                memory: &memory,
                view,
                copies: 0,
                accounted: 0,
            };
            let mut shifted = *d;
            shifted[0] -= base as u32;
            copy_texture_source_image_from(
                shifted,
                &mut usize::MAX.clone(),
                &mut TextureImageCache::default(),
                &mut source,
            )
            .unwrap();
        }
        start.elapsed().as_secs_f64() * 1e3
    };
    run(false); // first sight: decode and retain every image
    for _ in 0..3 {
        let copy = run(false);
        let view = run(true);
        println!(
            "warm update, {count} spans / {:.1} MiB: copy path {copy:.2} ms, in-place validation {view:.2} ms",
            (count * span) as f64 / 1048576.0
        );
    }
}
