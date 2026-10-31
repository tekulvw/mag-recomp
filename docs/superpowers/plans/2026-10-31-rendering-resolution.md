# Rendering resolution implementation plan

**Goal:** make a player setting increase GPU rasterization resolution independently of window/output size.

**Architecture:** wrap the existing Metal register-file backend at its registration boundary. Keep guest resource dimensions and memory layouts unchanged; scale host color/depth targets, viewport/scissor and partial clears together. Translate fragment window positions back to guest units, and resolve guest readbacks to their original dimensions. Ordinary textures retain their original size. The existing native-resolution path must be identical when scaling is disabled.

**User direction:** implement the improvement alongside the crash investigation; continue the existing compact UI. Use the existing isolated settings checkout. No game launch is needed for implementation or synthetic GPU verification.

## Constraints

- No game content, logs, private paths or captures in public source/tests.
- Preserve existing saved settings, server/account policy and the original SDK checkout/index.
- Rendering changes apply on the next launch; output resizing retains its live confirmation.
- Provide 100%, 150%, 200% and 300% scale, corresponding to the game's nominal 720p, 1080p, 1440p and 2160p heights.
- Preserve checked submission and failure reporting. Reject unsupported resource sizes explicitly; never silently fall back to output scaling.

## Work

- [x] Add a synthetic backend regression that fails without the adapter. Cover color/depth size, coordinates, seeded surfaces, snapshots, WPOS conversion and guest-sized readbacks.
- [x] Implement `src/player_render_scale.h/.cpp`; hook only Metal backend registration in `cmake/MagPlayerSettings.cmake`.
- [x] Add the independently saved rendering setting and launch snapshot to the existing store/UI/runtime, including old-preference migration and tests.
- [x] Verify on real Metal using synthetic geometry. Check actual target dimensions and pixels with more detail than a scaled native image.
- [x] Run the affected settings/CMake/archive checks and the public-tree check.
- [x] Rebuild the private app with the adapter and updated panel, preserving the canonical app.

The user subsequently authorized an in-game comparison with the earlier connection profile, followed by an in-game rendering check. The 200% check reached the main menu with 1280 × 720 color/depth targets allocated as 2560 × 1440 GPU targets and unchanged 1920 × 1080 output. Temporary preferences and the working no-ticket connection profile were used; saved preferences were unchanged. The owned bounded test was stopped after collecting this evidence. This is frontend acceptance, not mission rendering or performance acceptance. Private run evidence remains ignored.

## Review focus

- Fractional scaling must keep shared scissor boundaries and framebuffer dimensions aligned.
- Fragment window positions must be converted before existing antialias sample-grid/window-origin transforms.
- Seeded images, snapshots and readbacks must retain their API formats and guest dimensions.
- A saved window/fullscreen change must not accidentally change the running renderer scale.
- Oversized target allocations or invalid backend contracts must fail visibly.
