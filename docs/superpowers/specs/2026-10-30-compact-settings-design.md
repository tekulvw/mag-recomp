# Compact native settings

The user selected the compact native window after comparing it with a category sidebar and asked to implement it. Keep Connection and Graphics visible together. This scoped authorization resumes settings work only.

Use AppKit on macOS, the platform represented by the approved preview. Store versioned settings outside the installed game, with a blank server meaning the existing game configuration. Validate hostnames, IPv4 addresses and optional ports. Never change accounts or ticket policy. Apply a custom server through a private, disposable VFS overlay, preserving original files and all other archive members. Resolve hostnames to IPv4 before starting the client.

Expose windowed/borderless display, output size and the pinned runtime's existing Off/FXAA option. Keep scene rendering at its existing resolution. Output changes apply to the owned Metal window with a 15-second confirmation and rollback. AA and server changes made during play apply on the next launch; label that accurately. Hold game input while the panel owns focus, without pausing network simulation.

First launch opens settings before starting the guest. Later launches reuse the saved settings. Holding Option on launch opens setup for recovery; Command-comma and an application menu entry open it during play. Cancel, closing the panel, errors and display timeout must preserve the previous saved settings. Invalid settings must be recoverable before guest startup.

Alternatives considered: modifying the original retail Options menu (additional menu reverse engineering), a separate native panel (selected), and launcher-only settings (insufficient access during play). Compact was chosen over the category layout.

Verify with synthetic archives, native settings persistence/validation tests, a synthetic Metal window and installer packaging tests. Do not launch MAG, restart services, push, publish, modify the private submodule or touch its index. Full game acceptance remains subject to the project hold.
