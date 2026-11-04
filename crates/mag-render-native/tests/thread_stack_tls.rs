//! A thread created through `thread_stack::with_overhead` keeps the stack it
//! asked for although the process carries a static TLS block larger than
//! Rust's default 2 MiB thread stack, as the game runtime does. On glibc that
//! block comes out of every thread's stack; a plain size would overflow here.
//!
//! No libtest harness: its own threads would run with that same overhead.
use mag_render_native::thread_stack::with_overhead;

thread_local! {
    static LARGE: std::cell::RefCell<[u8; 3 << 20]> = const { std::cell::RefCell::new([0; 3 << 20]) };
}

#[inline(never)]
fn probe(depth: usize) -> usize {
    // About 8 KiB per frame, kept live past the recursion.
    let mut frame = [0u8; 8192];
    frame[depth % 8192] = depth as u8;
    let below = if depth == 0 { 0 } else { probe(depth - 1) };
    std::hint::black_box(&mut frame);
    below + usize::from(frame[depth % 8192])
}

fn main() {
    let usable = 8 << 20;
    let join = std::thread::Builder::new()
        .stack_size(with_overhead(usable))
        .spawn(move || {
            LARGE.with(|t| t.borrow_mut()[(3 << 20) - 1] = 1);
            // About three quarters of the usable stack.
            probe(usable * 3 / 4 / 8448)
        })
        .unwrap();
    assert!(join.join().is_ok());
    println!("ok: {} usable bytes beside 3 MiB of static TLS", usable);
}
