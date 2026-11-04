//! Cold source qualification shared by every host; never called per witness.
use sha2::{Digest, Sha256};
use std::fs::File;
use std::io::{self, Read};

const MAX_PATH: usize = 32_768;
const MAX_FILE: u64 = 1 << 30;
const INVALID: i32 = -1;
const IO_ERROR: i32 = -3;
const BUDGET: i32 = -5;

fn digest_reader(mut reader: impl Read, budget: u64) -> Result<[u8; 32], i32> {
    if budget == 0 || budget > MAX_FILE {
        return Err(INVALID);
    }
    let mut digest = Sha256::new();
    let mut total = 0_u64;
    let mut buffer = [0_u8; 65_536];
    loop {
        let size = match reader.read(&mut buffer) {
            Ok(0) => break,
            Ok(size) => size,
            Err(error) if error.kind() == io::ErrorKind::Interrupted => continue,
            Err(_) => return Err(IO_ERROR),
        };
        total = total.checked_add(size as u64).ok_or(BUDGET)?;
        if total > budget {
            return Err(BUDGET);
        }
        digest.update(&buffer[..size]);
    }
    Ok(digest.finalize().into())
}

fn file_digest(path: &[u8], budget: u64) -> Result<[u8; 32], i32> {
    if path.is_empty() || path.len() > MAX_PATH || path.contains(&0) {
        return Err(INVALID);
    }
    let path = std::str::from_utf8(path).map_err(|_| INVALID)?;
    let file = File::open(path).map_err(|_| IO_ERROR)?;
    let metadata = file.metadata().map_err(|_| IO_ERROR)?;
    if !metadata.is_file() {
        return Err(INVALID);
    }
    if budget == 0 || budget > MAX_FILE {
        return Err(INVALID);
    }
    if metadata.len() > budget {
        return Err(BUDGET);
    }
    digest_reader(file, budget)
}

/// SHA-256 of an actual regular file, at most 1 GiB, using locked RustCrypto.
/// Status 0 writes exactly 32 bytes; -1 invalid, -3 I/O, -5 byte budget exceeded.
/// An error leaves the output unchanged. No pointer is retained.
///
/// # Safety
/// `path` must address `path_length` readable bytes; `output` must address 32
/// writable bytes. Paths are UTF-8 without a NUL; their lifetime spans this call.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn mag_original_work_sha256_file(
    path: *const u8,
    path_length: u32,
    budget: u64,
    output: *mut u8,
) -> i32 {
    if path.is_null() || output.is_null() || path_length == 0 || path_length as usize > MAX_PATH {
        return INVALID;
    }
    // SAFETY: caller supplies this bounded readable range for this call only.
    let path = unsafe { std::slice::from_raw_parts(path, path_length as usize) };
    match file_digest(path, budget) {
        Ok(digest) => {
            // SAFETY: output is a caller-owned 32-byte writable range; digest
            // is a distinct local array. The input path is no longer accessed.
            unsafe { std::ptr::copy_nonoverlapping(digest.as_ptr(), output, digest.len()) };
            0
        }
        Err(status) => status,
    }
}

#[cfg(test)]
mod tests {
    use super::*;
    use std::io::Cursor;
    use std::sync::atomic::{AtomicU64, Ordering};

    fn hex(bytes: [u8; 32]) -> String {
        bytes.iter().map(|byte| format!("{byte:02x}")).collect()
    }

    #[test]
    fn standard_vectors_cross_streaming_boundary() {
        assert_eq!(
            hex(digest_reader(Cursor::new([]), 1).unwrap()),
            "e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855"
        );
        assert_eq!(
            hex(digest_reader(Cursor::new(b"abc"), 3).unwrap()),
            "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad"
        );
        assert_eq!(
            hex(digest_reader(Cursor::new(vec![b'a'; 1_000_000]), 1_000_000).unwrap()),
            "cdc76e5c9914fb9281a1c7e284d73e67f1809a48a497200e046d39ccc7112cd0"
        );
    }

    #[test]
    fn bounded_reader_refuses_overrun_and_io_failure() {
        assert_eq!(digest_reader(Cursor::new(b"abc"), 2), Err(BUDGET));
        assert_eq!(digest_reader(Cursor::new([]), MAX_FILE + 1), Err(INVALID));
        struct Broken;
        impl Read for Broken {
            fn read(&mut self, _: &mut [u8]) -> io::Result<usize> {
                Err(io::Error::other("authored read failure"))
            }
        }
        assert_eq!(digest_reader(Broken, 10), Err(IO_ERROR));
    }

    #[test]
    fn actual_file_ffi_preserves_output_on_error() {
        static NEXT: AtomicU64 = AtomicU64::new(0);
        let (path, mut file) = loop {
            let path = std::env::temp_dir().join(format!(
                "mag-original-hash-{}-{}",
                std::process::id(),
                NEXT.fetch_add(1, Ordering::Relaxed)
            ));
            match std::fs::OpenOptions::new()
                .write(true)
                .create_new(true)
                .open(&path)
            {
                Ok(file) => break (path, file),
                Err(error) if error.kind() == io::ErrorKind::AlreadyExists => continue,
                Err(error) => panic!("create owned hash input: {error}"),
            }
        };
        use std::io::Write;
        file.write_all(b"abc").unwrap();
        drop(file);
        let text = path.to_str().unwrap().as_bytes();
        let mut out = [0xcc; 32];
        // SAFETY: live owned path bytes and a 32-byte output array.
        assert_eq!(
            unsafe {
                mag_original_work_sha256_file(text.as_ptr(), text.len() as u32, 2, out.as_mut_ptr())
            },
            BUDGET
        );
        assert_eq!(out, [0xcc; 32]);
        assert_eq!(
            unsafe {
                mag_original_work_sha256_file(text.as_ptr(), text.len() as u32, 3, out.as_mut_ptr())
            },
            0
        );
        assert_eq!(
            hex(out),
            "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad"
        );
        std::fs::remove_file(&path).unwrap();
        out = [0xcc; 32];
        assert_eq!(
            unsafe {
                mag_original_work_sha256_file(text.as_ptr(), text.len() as u32, 3, out.as_mut_ptr())
            },
            IO_ERROR
        );
        assert_eq!(out, [0xcc; 32]);
        assert_eq!(file_digest(&[0xff], 3), Err(INVALID));
        assert_eq!(file_digest(b"bad\0path", 3), Err(INVALID));
    }
}
