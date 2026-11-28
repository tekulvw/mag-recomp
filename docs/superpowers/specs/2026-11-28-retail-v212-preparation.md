# Offline retail 2.12 installation preparation

Prepare an independent US disc installation from the original BCUS98110 v1.0 disc,
the official 02.12 update PKG, and four installed patch PSARCs. This is data preparation;
retail v1.0 remains the only supported native build.

Use a standalone Python CLI and a versioned identity profile containing paths, sizes,
hashes and patch dependencies only. Require every original disc file in the profile,
all 15 update files, and the four disc-edition patches. Ignore unlisted input extras.
Accept either the original PKG, extracted using the existing pinned SDK helper, or an
already extracted package tree with identical bytes. Do not embed keys or game content.

Write `disc/` and `dev_hdd0/game/BCUS98110/` beneath a new output directory. Copy the
disc settings archive into installed USRDIR and retain the patches in chain order.
Keep encrypted executables and SDATs intact. Do not flatten the patch overlays, add
optional DLC, copy accounts, configure an emulator, or launch the game.

Validate hashes and embedded patch dependencies before finalizing. Verify staged
output against the shipped profile, then publish it by renaming the staging directory.
Reject existing or overlapping outputs and symlink inputs. Use independent copies
(APFS clones when available), never hardlinks. A separate verify command must detect
missing, modified or unexpected files without trusting the receipt's file hashes.

Receipt claims are limited to an offline verified input/layout set at APP_VER 02.12,
content level 489979. Runtime acceptance and native 2.12 support remain untested.
