//! Persistent helper threads for synchronous capture work.
//!
//! `PS3_RENDER_CAPTURE_POOL=1` (default off) replaces the scoped threads that
//! the capture spawns on every update with a small set of long-lived ones.
//! [`join`] keeps the scoped contract: it returns, or unwinds, only after every
//! job it was given has finished, so jobs may borrow the caller's data. The
//! calling thread runs its own work first and then drains any job no helper
//! has started, so a busy or missing helper only costs parallelism.
use std::{
    any::Any,
    collections::VecDeque,
    panic::{AssertUnwindSafe, catch_unwind, resume_unwind},
    sync::{Arc, Condvar, Mutex, OnceLock},
};

pub fn enabled() -> bool {
    static F: OnceLock<bool> = OnceLock::new();
    *F.get_or_init(|| std::env::var_os("PS3_RENDER_CAPTURE_POOL").is_some_and(|v| v == "1"))
}

type Job = Box<dyn FnOnce() + Send + 'static>;
struct Batch {
    jobs: Mutex<VecDeque<Job>>,
    /// Jobs not yet finished, and the first panic payload.
    state: Mutex<(usize, Option<Box<dyn Any + Send>>)>,
    finished: Condvar,
}
impl Batch {
    /// Run one queued job of this batch, if any is left. Returns whether it ran.
    fn run_one(&self) -> bool {
        let Some(job) = self.jobs.lock().ok().and_then(|mut j| j.pop_front()) else {
            return false;
        };
        let outcome = catch_unwind(AssertUnwindSafe(job));
        let mut state = self.state.lock().unwrap_or_else(|e| e.into_inner());
        state.0 -= 1;
        if let Err(payload) = outcome {
            state.1.get_or_insert(payload);
        }
        if state.0 == 0 {
            self.finished.notify_all();
        }
        true
    }
}
struct Pool {
    queue: Mutex<VecDeque<Arc<Batch>>>,
    wake: Condvar,
    helpers: usize,
}
fn pool() -> &'static Pool {
    static POOL: OnceLock<&'static Pool> = OnceLock::new();
    POOL.get_or_init(|| {
        let helpers = std::thread::available_parallelism()
            .map_or(1, |n| n.get())
            .saturating_sub(1)
            .clamp(1, 7);
        let pool: &'static Pool = Box::leak(Box::new(Pool {
            queue: Mutex::new(VecDeque::new()),
            wake: Condvar::new(),
            helpers,
        }));
        for i in 0..helpers {
            // A helper that cannot start only removes parallelism: callers
            // drain their own batches.
            let _ = std::thread::Builder::new()
                .name(format!("mag-capture-pool-{i}"))
                .spawn(move || {
                    loop {
                        let batch = {
                            let mut queue = pool.queue.lock().unwrap_or_else(|e| e.into_inner());
                            loop {
                                if let Some(batch) = queue.pop_front() {
                                    break batch;
                                }
                                queue = pool.wake.wait(queue).unwrap_or_else(|e| e.into_inner());
                            }
                        };
                        batch.run_one();
                    }
                });
        }
        pool
    })
}
/// Number of helper threads a caller can expect in addition to itself.
pub fn helpers() -> usize {
    pool().helpers
}
/// Run `local` on the calling thread while helpers run `jobs`; return once
/// every job has finished. A panic in a job or in `local` is resumed only after
/// all jobs have finished.
pub fn join<'a, R>(jobs: Vec<Box<dyn FnOnce() + Send + 'a>>, local: impl FnOnce() -> R) -> R {
    let count = jobs.len();
    // SAFETY: the jobs may borrow data that lives for 'a. This function does
    // not return or unwind before `remaining` reaches zero, i.e. before every
    // job has run to completion (or unwound) and been dropped, so no job
    // outlives the borrows it captured.
    let jobs: VecDeque<Job> = jobs
        .into_iter()
        .map(|job| unsafe { std::mem::transmute::<Box<dyn FnOnce() + Send + 'a>, Job>(job) })
        .collect();
    let batch = Arc::new(Batch {
        jobs: Mutex::new(jobs),
        state: Mutex::new((count, None)),
        finished: Condvar::new(),
    });
    if count > 0 {
        let pool = pool();
        let mut queue = pool.queue.lock().unwrap_or_else(|e| e.into_inner());
        for _ in 0..count {
            queue.push_back(Arc::clone(&batch));
        }
        drop(queue);
        pool.wake.notify_all();
    }
    let local = catch_unwind(AssertUnwindSafe(local));
    while batch.run_one() {}
    let panic = {
        let mut state = batch.state.lock().unwrap_or_else(|e| e.into_inner());
        while state.0 != 0 {
            state = batch
                .finished
                .wait(state)
                .unwrap_or_else(|e| e.into_inner());
        }
        state.1.take()
    };
    match (local, panic) {
        (Err(payload), _) | (Ok(_), Some(payload)) => resume_unwind(payload),
        (Ok(value), None) => value,
    }
}

#[cfg(test)]
mod tests {
    use super::*;
    use std::sync::atomic::{AtomicUsize, Ordering};
    #[test]
    fn jobs_borrow_caller_data_and_all_finish_before_return() {
        let mut slots = [0usize; 16];
        let ran = AtomicUsize::new(0);
        let local = join(
            slots
                .iter_mut()
                .enumerate()
                .map(|(i, slot)| {
                    let ran = &ran;
                    Box::new(move || {
                        *slot = i * 3;
                        ran.fetch_add(1, Ordering::SeqCst);
                    }) as Box<dyn FnOnce() + Send + '_>
                })
                .collect(),
            || 7,
        );
        assert_eq!(local, 7);
        assert_eq!(ran.load(Ordering::SeqCst), 16);
        assert!(slots.iter().enumerate().all(|(i, &v)| v == i * 3));
    }
    #[test]
    fn a_job_panic_is_resumed_after_every_job_finished() {
        let ran = AtomicUsize::new(0);
        let result = catch_unwind(AssertUnwindSafe(|| {
            join(
                (0..8)
                    .map(|i| {
                        let ran = &ran;
                        Box::new(move || {
                            if i == 3 {
                                panic!("job");
                            }
                            ran.fetch_add(1, Ordering::SeqCst);
                        }) as Box<dyn FnOnce() + Send + '_>
                    })
                    .collect(),
                || (),
            )
        }));
        assert!(result.is_err());
        assert_eq!(ran.load(Ordering::SeqCst), 7);
    }
    #[test]
    fn nested_and_concurrent_joins_complete() {
        let total = AtomicUsize::new(0);
        std::thread::scope(|scope| {
            for _ in 0..4 {
                scope.spawn(|| {
                    join(
                        (0..6)
                            .map(|_| {
                                let total = &total;
                                Box::new(move || {
                                    join(
                                        vec![Box::new(move || {
                                            total.fetch_add(1, Ordering::SeqCst);
                                        })
                                            as Box<dyn FnOnce() + Send + '_>],
                                        || (),
                                    );
                                }) as Box<dyn FnOnce() + Send + '_>
                            })
                            .collect(),
                        || (),
                    )
                });
            }
        });
        assert_eq!(total.load(Ordering::SeqCst), 24);
    }
}
