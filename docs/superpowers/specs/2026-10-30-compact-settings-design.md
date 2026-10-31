# Compact native settings

The user selected the compact native window after comparing it with a category sidebar and asked to implement it. Keep Connection and Graphics visible together. This scoped authorization resumes settings work only.

Use AppKit on macOS, the platform represented by the approved preview. Store versioned settings outside the installed game, with a blank server meaning the existing game configuration. Validate hostnames, IPv4 addresses and optional ports. Never change accounts. Apply a custom server through a private, disposable VFS overlay, preserving original files and all other archive members. Resolve hostnames to IPv4 before starting the client.

The initial design preserved the retail ticket policy. On October 31 the user requested a comparison with the earlier working connection settings, confirmed that it worked, and requested a playable test client. A selected server therefore uses the established community profile with PSN tickets disabled; the original account choices are retained. Blank server continues to use the original connection policy.

Expose windowed/borderless display, output size and the pinned runtime's existing Off/FXAA option. Keep scene rendering at its existing resolution. Output changes apply to the owned Metal window with a 15-second confirmation and rollback. AA and server changes made during play apply on the next launch; label that accurately. Hold game input while the panel owns focus, without pausing network simulation.

First launch opens settings before starting the guest. Later launches reuse the saved settings. Holding Option on launch opens setup for recovery; Command-comma and an application menu entry open it during play. Cancel, closing the panel, errors and display timeout must preserve the previous saved settings. Invalid settings must be recoverable before guest startup.

Alternatives considered: modifying the original retail Options menu (additional menu reverse engineering), a separate native panel (selected), and launcher-only settings (insufficient access during play). Compact was chosen over the category layout.

Verify with synthetic archives, native settings persistence/validation tests, a synthetic Metal window and installer packaging tests. The initial settings authorization excluded game runs and publication. On October 31 the user separately authorized bounded connection/rendering tests, these settings and resolution PRs, and a local playable test client. Preserve the private submodule and native index; unrelated launches, service changes and publication remain subject to the project hold.
