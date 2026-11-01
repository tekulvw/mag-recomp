#pragma once
#include "player_controls.h"
#include <filesystem>
#include <span>
namespace mag {
// The runtime supplies its supported action catalog; the portable store never
// guesses game-specific identifiers. Missing profiles return defaults; malformed
// or unsupported stored data throws and remains on disk for explicit recovery.
ControlsProfile mag_controls_load_profile(const std::filesystem::path&, std::span<const ActionId> catalog);
// Failure preserves the previous file. Does not configure active input.
bool mag_controls_save_profile(const std::filesystem::path&, const ControlsProfile&, std::span<const ActionId> catalog);
}
