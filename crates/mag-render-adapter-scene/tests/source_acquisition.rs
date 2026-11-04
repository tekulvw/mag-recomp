//! Opt-in acquisition reuse with every switch enabled for this test process.
//! Default-off behaviour is guarded by the existing suites, which assert that
//! callback counts equal the logical per-phase counters.
use core::ffi::c_void;
use mag_render_adapter_scene::{
    cohort_ffi::*,
    cohort_resource_ffi::*,
    error::Error,
    material_textures::{
        LocatedTextureRequest, TextureError, TextureImageCache, copy_texture_source_image,
    },
    source_acquisition,
};
use std::sync::{Arc, Mutex};

static LOCK: Mutex<()> = Mutex::new(());
fn enable() -> std::sync::MutexGuard<'static, ()> {
    // SAFETY: every test in this process sets the same value under one lock
    // before any adapter call, so no thread observes a concurrent change.
    let guard = LOCK.lock().unwrap_or_else(|e| e.into_inner());
    unsafe {
        std::env::set_var("PS3_RENDER_RESIDENT_SOURCES", "1");
        std::env::set_var("PS3_RENDER_CAPTURE_READ_SHARE", "1");
        std::env::set_var("PS3_RENDER_ACQUISITION_PROFILE", "1");
    }
    assert!(source_acquisition::resident_textures_enabled());
    assert!(source_acquisition::read_share_enabled());
    assert!(source_acquisition::content_probe_enabled());
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
/// One update's texture acquisition, with a fresh capture-local cache exactly
/// like every live capture. Returns the image and the number of reads made.
fn update(
    descriptor: [u32; 8],
    guest: &[u8],
) -> (
    Result<Arc<mag_render_adapter_scene::material_textures::OwnedTextureImage>, TextureError>,
    usize,
) {
    let mut reads = 0;
    let mut remaining = usize::MAX;
    let image = copy_texture_source_image(
        descriptor,
        &mut remaining,
        &mut TextureImageCache::default(),
        &mut |r: LocatedTextureRequest| {
            reads += 1;
            if guest.is_empty() {
                return Err(TextureError::Source(Error::OutsideSnapshot));
            }
            Ok(guest[r.offset as usize..r.offset as usize + r.bytes].to_vec())
        },
    );
    (image, reads)
}

#[test]
fn resident_texture_survives_updates_and_follows_mutation_free_and_reload() {
    let _guard = enable();
    let descriptor = bc1(0x1000, 256, 256); // 32 KiB BC1 span
    let span = 256 * 256 / 2;
    let mut guest = vec![0u8; 0x1000 + span];
    for (i, b) in guest[0x1000..].iter_mut().enumerate() {
        *b = (i * 13 + 1) as u8;
    }
    let before = source_acquisition::thread_stats();
    let (first, reads) = update(descriptor, &guest);
    let first = first.unwrap();
    assert_eq!(reads, 1);
    // Next update: still exactly one counted read, same immutable version.
    let (second, reads) = update(descriptor, &guest);
    assert_eq!(reads, 1);
    assert!(Arc::ptr_eq(&first, &second.unwrap()));
    // In-place mutation of one texel block: a fresh decode with new content.
    guest[0x1000 + span / 2] ^= 0x55;
    let (mutated, reads) = update(descriptor, &guest);
    let mutated = mutated.unwrap();
    assert_eq!(reads, 1);
    assert!(!Arc::ptr_eq(&first, &mutated));
    assert_eq!(mutated.source_bytes, guest[0x1000..]);
    assert_eq!(mutated.levels[0].bytes, guest[0x1000..]);
    // The earlier version, still held by an in-flight consumer, is unchanged.
    assert_ne!(first.source_bytes, mutated.source_bytes);
    // Freed range: the read itself fails, and no retained image is returned.
    let (freed, reads) = update(descriptor, &[]);
    assert_eq!(reads, 1);
    assert!(freed.is_err());
    // Reload of the original asset at the same location: content qualifies.
    guest[0x1000 + span / 2] ^= 0x55;
    let (reloaded, _) = update(descriptor, &guest);
    let reloaded = reloaded.unwrap();
    assert_eq!(reloaded.source_bytes, first.source_bytes);
    let (again, _) = update(descriptor, &guest);
    assert!(Arc::ptr_eq(&reloaded, &again.unwrap()));
    let stats = source_acquisition::thread_stats().since(before);
    let profile = source_acquisition::thread_profile();
    assert!(profile[source_acquisition::Section::TextureImages as usize] > 0);
    assert_eq!(
        profile[source_acquisition::Section::GroupZeroPreparation as usize],
        0
    );
    assert_eq!(stats.resident_hits, 2);
    assert_eq!(stats.resident_invalidations, 2); // mutation, then reload
    assert_eq!(stats.resident_misses, 1);
}

#[derive(Default)]
struct Counts {
    reads: u64,
    bytes: u64,
}
fn source_bytes(address: u32) -> Option<Vec<u8>> {
    let word = |b: &mut [u8], at: usize, v: u32| b[at..at + 4].copy_from_slice(&v.to_be_bytes());
    Some(match address {
        0x10000 => {
            let mut m = vec![0; 128];
            m[0x48..0x4a].copy_from_slice(&0x60u16.to_be_bytes());
            m[0x4a..0x4c].copy_from_slice(&0x50u16.to_be_bytes());
            word(&mut m, 0x38, 0x30000);
            word(&mut m, 0x54, 16);
            m
        }
        0x20000 => vec![0; 192],
        0x30000 => vec![0x31; 16],
        0x40000 => vec![0; 544],
        0x80000 => {
            let mut pass = vec![0; 688];
            word(&mut pass, 0x284, 0x40000);
            word(&mut pass, 0x1fc, 0x80000);
            pass
        }
        _ => return None,
    })
}
unsafe extern "C" fn copy(opaque: *mut c_void, address: u32, bytes: u32, out: *mut u8) -> i32 {
    let Some(owned) = source_bytes(address) else {
        return -1;
    };
    if owned.len() != bytes as usize || out.is_null() {
        return -1;
    }
    // SAFETY: production synchronous copy ABI; opaque is a live Counts.
    unsafe {
        std::ptr::copy_nonoverlapping(owned.as_ptr(), out, owned.len());
        let counts = &mut *opaque.cast::<Counts>();
        counts.reads += 1;
        counts.bytes += u64::from(bytes);
    }
    0
}

#[test]
fn shared_capture_reads_keep_logical_counters_and_report_actual_callbacks() {
    // The adapter entry points do nothing while the master switch is off.
    mag_render_adapter_core::runtime::enable();
    let _guard = enable();
    assert_eq!(mag_render_adapter_cohort_vm(61), 0);
    let items = 4u32;
    let mut begin = CohortBegin {
        abi_version: 1,
        struct_bytes: size_of::<CohortBegin>() as u32,
        vm: 61,
        invocation: 1,
        owner: 11,
        pass_occurrence: 9,
        execution_update: 27,
        ..CohortBegin::default()
    };
    begin.group_counts[0][15] = items;
    assert_eq!(mag_render_adapter_cohort_begin(begin), 0);
    for _ in 0..items {
        let mut item = CohortItemInput {
            vm: 61,
            invocation: 1,
            source_index: 17,
            has_transform: 1,
            ..CohortItemInput::default()
        };
        // Identical placements of one mesh/material: the repeated case.
        item.mbi = [0xf000_0000, 1, 0, 0x2000, 0, 8, 0, 8];
        assert_eq!(mag_render_adapter_cohort_item(item), 0);
    }
    assert_eq!(mag_render_adapter_cohort_finish(61, 1), 0);
    let input = CohortResourceInput {
        abi_version: 2,
        struct_bytes: size_of::<CohortResourceInput>() as u32,
        vm: 61,
        invocation: 1,
        source_pass_address: 0x80000,
        geometry_resource_base: 0x40000,
        ..CohortResourceInput::default()
    };
    let mut counts = Counts::default();
    // SAFETY: complete input and a live synchronous callback/opaque.
    let result =
        unsafe { mag_render_adapter_cohort_assets(input, Some(copy), (&raw mut counts).cast()) };
    assert_eq!(result.status, 0);
    // Logical requests are unchanged by sharing: 1 pass + 4 per item.
    assert_eq!(result.reads, 1 + 4 * u64::from(items));
    assert_eq!(
        result.bytes,
        688 + u64::from(items) * (128 + 192 + 16 + 544)
    );
    let owned = take_latest_main_sources().unwrap();
    let stats = owned.acquisition;
    // Every callback is reported; each range is fetched twice (the second
    // fetch is copied for reuse) and later identical requests are shared.
    assert_eq!(
        (stats.guest_callbacks, stats.guest_callback_bytes),
        (counts.reads, counts.bytes)
    );
    assert_eq!(counts.reads, 1 + 4 * 2);
    assert_eq!(stats.shared_reads, 4 * (u64::from(items) - 2));
    assert_eq!(
        stats.guest_callbacks + stats.shared_reads,
        result.reads,
        "every logical request is either a callback or a shared copy"
    );
    // Shared items carry identical owned bytes.
    let resources = owned.resources.as_ref().unwrap();
    for item in &resources.items {
        let material = item.primary_material.as_ref().unwrap();
        assert_eq!(material.bytes(), source_bytes(0x10000).unwrap());
        assert_eq!(
            item.partitions.as_ref().unwrap().partitions()[0].bytes()[..],
            source_bytes(0x40000).unwrap()[..]
        );
    }
}
