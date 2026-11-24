#pragma once
// The game's side of friends and presence (src/mag_social_bridge.cpp, docs/player-social.md).
#ifdef __cplusplus
#include "player_social.h"
#include <chrono>
#include <filesystem>
#include <functional>
#include <memory>
#include <string>

namespace mag::social {
// Pass one worker output to the runtime (an NP Basic event, or a finished lookup).
void deliver(const Output &output);
// Linux and Windows: the supervisor's spool folder. False when it is missing or a bridge exists.
bool install_spool_bridge(const std::filesystem::path &dir, std::chrono::milliseconds poll = std::chrono::milliseconds(200));
// macOS: run the worker in this process with the device token in `link_file` for `portal`.
// False (quietly) when not linked to that portal.
bool install_in_process(std::function<std::unique_ptr<portal::Transport>()> transport, const std::filesystem::path &link_file,
                        const std::string &portal, ServiceOptions tuning = {});
}  // namespace mag::social
extern "C" {
#endif
// From main(), in the engine, after the player settings are applied and before the game starts.
// Reads MAG_SOCIAL, MAG_SOCIAL_SPOOL (and on macOS MAG_SOCIAL_PORTAL and MAG_SOCIAL_LINK_FILE).
void mag_social_start(void);
#ifdef __cplusplus
}
#endif
