# Rendering frontend ABI records

This dependency contains only C-compatible Rust records used by the owned
scene packet and its shared descriptors. A private source adapter can construct
packets without linking a second native renderer or shader compiler.

The public C contract is `src/mag_render_scene_packet.h` and the native headers
it includes. The native crate executes an independent C/Rust check for every
packet field, plus the existing host image/frame and descriptor layout checks.
This crate does not validate native pointer provenance or grant ownership;
submission still requires the actual synchronous copy, host pinning and
reservation protocol. Runtime capacities are queried from the native provider.
