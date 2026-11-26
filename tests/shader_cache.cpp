// Synthetic checks of the Windows shader cache default (src/mag_shader_cache.h).
#include "mag_shader_cache.h"
#include <cstdio>
#include <string>
#include <vector>

static int failures = 0;
#define CHECK(c) do { if (!(c)) { std::fprintf(stderr, "%s:%d: CHECK(%s)\n", __FILE__, __LINE__, #c); ++failures; } } while (0)

int main() {
    std::vector<std::string> made;
    auto ok = [&](const std::string &d) { made.push_back(d); return true; };
    auto fail = [&](const std::string &d) { made.push_back(d); return false; };

    auto c = mag::choose_shader_cache(nullptr, nullptr, "C:\\Users\\P\\AppData\\Local", ok);
    CHECK(c.dir == "C:\\Users\\P\\AppData\\Local\\MAG\\cache\\shaders");
    CHECK(made.size() == 1 && made[0] == c.dir);
    CHECK(c.why == "shader cache: " + c.dir);

    made.clear();
    c = mag::choose_shader_cache(nullptr, nullptr, "C:\\Local\\", ok);  // a trailing separator is not doubled
    CHECK(c.dir == "C:\\Local\\MAG\\cache\\shaders");

    made.clear();
    c = mag::choose_shader_cache("D:\\mine", nullptr, "C:\\Local", ok);  // the player's own value wins
    CHECK(c.dir.empty() && made.empty() && c.why.find("kept") != std::string::npos);
    c = mag::choose_shader_cache("", nullptr, "C:\\Local", ok);           // even an empty one
    CHECK(c.dir.empty() && made.empty());

    c = mag::choose_shader_cache(nullptr, "0", "C:\\Local", ok);          // the off switch
    CHECK(c.dir.empty() && made.empty() && c.why == "shader cache: off (MAG_SHADER_CACHE=0)");
    c = mag::choose_shader_cache(nullptr, "1", "C:\\Local", ok);          // anything else keeps it on
    CHECK(!c.dir.empty());

    made.clear();
    c = mag::choose_shader_cache(nullptr, nullptr, nullptr, ok);          // no LOCALAPPDATA: off, said so
    CHECK(c.dir.empty() && made.empty() && c.why == "shader cache: off (no LOCALAPPDATA)");
    c = mag::choose_shader_cache(nullptr, nullptr, "", ok);
    CHECK(c.dir.empty() && made.empty());

    c = mag::choose_shader_cache(nullptr, nullptr, "C:\\Local", fail);    // cannot create: off, said so
    CHECK(c.dir.empty() && c.why.find("cannot create") != std::string::npos);

    if (failures) { std::fprintf(stderr, "%d failures\n", failures); return 1; }
    std::puts("[shader_cache] PASS");
    return 0;
}
