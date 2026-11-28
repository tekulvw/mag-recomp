#pragma once
#include <filesystem>
#include <string>

namespace mag {
struct V212BootPaths {
    std::string disc_root;
    std::string hdd_root;
    std::string paramsfo;
};

inline bool v212_boot_paths(const char* disc, const char* hdd,
                           V212BootPaths& output, std::string& error) {
    if (!disc || !*disc || !hdd || !*hdd) {
        error = "2.12 requires explicit PS3_VFS_ROOT and PS3_HDD0_ROOT";
        return false;
    }
    std::error_code ec;
    const auto disc_root = std::filesystem::canonical(disc, ec);
    if (ec || !std::filesystem::is_directory(disc_root / "PS3_GAME", ec)) {
        error = "2.12 disc root must contain PS3_GAME";
        return false;
    }
    const auto hdd_root = std::filesystem::canonical(hdd, ec);
    const auto sfo = hdd_root / "game/BCUS98110/PARAM.SFO";
    if (ec || !std::filesystem::is_regular_file(sfo, ec)) {
        error = "2.12 HDD root must contain game/BCUS98110/PARAM.SFO";
        return false;
    }
    output = {disc_root.string(), hdd_root.string(), sfo.string()};
    return true;
}
}
