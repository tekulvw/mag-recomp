//! Bounded target-independent CPU preparations. An admission is not a GPU
//! ticket, original job output publication, or suppression certificate.
use super::*;
pub use mag_render_abi::packet::Admission;
use std::sync::atomic::{AtomicU64, Ordering};
static NEXT: AtomicU64 = AtomicU64::new(1);
fn source_key(s: Source) -> [u64; 4] {
    [s.engine_update, s.cohort, s.publication, s.pass_occurrence]
}
pub(crate) fn same_source(a: Source, b: Source) -> bool {
    source_key(a) == source_key(b)
}
fn signature(a: Admission) -> (u32, u32, u64, u64, u64, [u64; 4], u64, u64, u32, u32) {
    (
        a.abi_version,
        a.struct_size,
        a.worker,
        a.token,
        a.native_generation,
        source_key(a.source),
        a.intended_items,
        a.draws,
        a.flags,
        a.reserved,
    )
}
pub(super) struct Prepared {
    pub commands: std::rc::Rc<SceneCommands>,
    pub leases: Vec<ResourceLease>,
    pub pipelines: Vec<u64>,
    pub output: ImageDesc,
    pub receipt: Receipt,
    pub identities: Vec<DrawIdentity>,
}
struct Entry<T> {
    admission: Admission,
    value: T,
}
pub(crate) struct Transactions<T> {
    entries: BTreeMap<u64, Entry<T>>,
}
impl<T> Default for Transactions<T> {
    fn default() -> Self {
        Self {
            entries: BTreeMap::new(),
        }
    }
}
impl<T> Transactions<T> {
    pub(crate) fn len(&self) -> usize {
        self.entries.len()
    }
    pub(crate) fn insert(
        &mut self,
        owner: (u64, u64),
        source: Source,
        intended: u64,
        draws: u64,
        value: T,
        other_slots: usize,
    ) -> Result<Admission, i32> {
        let (worker, generation) = owner;
        if self
            .entries
            .len()
            .checked_add(other_slots)
            .is_none_or(|n| n >= 2)
        {
            return Err(-4);
        }
        if worker == 0
            || generation == 0
            || source.cohort == 0
            || source.publication == 0
            || source.pass_occurrence == 0
        {
            return Err(-1);
        }
        let token = NEXT
            .try_update(Ordering::Relaxed, Ordering::Relaxed, |v| v.checked_add(1))
            .map_err(|_| -5)?;
        let admission = Admission {
            abi_version: 1,
            struct_size: size_of::<Admission>() as u32,
            worker,
            token,
            native_generation: generation,
            source,
            intended_items: intended,
            draws,
            flags: 1,
            reserved: 0,
        };
        self.entries.insert(token, Entry { admission, value });
        Ok(admission)
    }
    pub(crate) fn get(&self, a: Admission, source: Source) -> Result<&T, i32> {
        let e = self.entries.get(&a.token).ok_or(-3)?;
        if signature(a) != signature(e.admission) || !same_source(source, a.source) {
            return Err(-3);
        }
        Ok(&e.value)
    }
    pub(crate) fn remove(&mut self, a: Admission, source: Source) -> Result<T, i32> {
        self.get(a, source)?;
        Ok(self.entries.remove(&a.token).ok_or(-3)?.value)
    }
}
pub(crate) fn checked_layout() -> Result<(), i32> {
    unsafe extern "C" {
        fn mag_native_admission_layout(out: *mut u32, words: u32) -> i32;
    }
    let expected = mag_render_abi::packet::admission_layout();
    let mut actual = vec![0; expected.len()];
    let status = unsafe { mag_native_admission_layout(actual.as_mut_ptr(), actual.len() as u32) };
    if status != 0 {
        return Err(status);
    }
    if actual != expected {
        return Err(-1);
    }
    Ok(())
}

#[cfg(test)]
mod tests {
    use super::*;
    use std::{cell::Cell, rc::Rc};
    fn source() -> Source {
        Source {
            engine_update: 1,
            cohort: 2,
            publication: 3,
            pass_occurrence: 4,
        }
    }
    struct Pin(Rc<Cell<usize>>);
    impl Drop for Pin {
        fn drop(&mut self) {
            self.0.set(self.0.get() - 1);
        }
    }
    fn pin(v: &Rc<Cell<usize>>) -> Pin {
        v.set(v.get() + 1);
        Pin(v.clone())
    }
    #[test]
    fn independent_admission_c_layout_matches() {
        checked_layout().unwrap();
    }
    #[test]
    fn target_free_admission_keeps_owned_resources_until_explicit_cancel() {
        let count = Rc::new(Cell::new(0));
        let mut t = Transactions::default();
        let a = t.insert((1, 7), source(), 3, 4, pin(&count), 0).unwrap();
        assert_eq!(count.get(), 1);
        assert_eq!(a.native_generation, 7);
        assert_eq!(a.flags, 1);
        assert!(t.get(a, source()).is_ok());
        drop(t.remove(a, source()).unwrap());
        assert_eq!(count.get(), 0);
        assert!(t.get(a, source()).is_err());
    }
    #[test]
    fn wrong_source_worker_context_or_replay_never_consumes_preparation() {
        let mut t = Transactions::default();
        let a = t.insert((1, 7), source(), 3, 4, 19, 0).unwrap();
        let mut bad = a;
        bad.source.publication += 1;
        assert!(t.remove(bad, bad.source).is_err());
        bad = a;
        bad.worker = 2;
        assert!(t.remove(bad, source()).is_err());
        bad = a;
        bad.native_generation = 8;
        assert!(t.remove(bad, source()).is_err());
        assert_eq!(*t.get(a, source()).unwrap(), 19);
        assert_eq!(t.remove(a, source()).unwrap(), 19);
        assert!(t.remove(a, source()).is_err());
    }
    #[test]
    fn prepared_pending_and_ready_share_exact_two_slots() {
        let mut t = Transactions::default();
        let a = t.insert((1, 7), source(), 3, 4, 1, 1).unwrap();
        assert!(t.insert((1, 7), source(), 3, 4, 2, 1).is_err());
        assert_eq!(t.len(), 1);
        t.remove(a, source()).unwrap();
        assert!(t.insert((1, 7), source(), 3, 4, 2, 2).is_err());
    }
}
