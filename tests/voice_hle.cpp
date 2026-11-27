// The voice service's guest glue against a stand-in runtime: registration only when switched on,
// and the public parameter and info blocks read and written big-endian. Synthetic; no game data.
//   voice_hle disabled   expects no handler registered
//   voice_hle pattern    expects the handlers and drives a port graph through them
//   voice_hle tone       the same with Opus, and the received talker read back as PCM
//   voice_hle secondary  the same with Opus, the talker on the headset output as the game connects
//                        it; the host stream (a silent clock in tests) plays it
//   voice_hle ptt        MAG_VOICE_MODE=ptt with a stand-in microphone (VOICE_TEST_MIC=tone, or
//                        none for a missing one) and stand-in push to talk: blocks only while held
//   voice_hle receive    MAG_VOICE_MODE=ptt without push to talk: registered, microphone never opened
#include "mag_extension.h"
#include "mag_voice_capture.h"
#include "ppu/ppu_context.h"
#include "ps3emu/nid.h"
#if MAG_VOICE_PUSH_TO_TALK
#include "mag_game_controls_push_to_talk.h"
#endif

#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdlib>
#include <cstdio>
#include <cstring>
#include <string>
#include <thread>
#include <vector>

extern "C" {
uint8_t* vm_base = nullptr;
}

struct Registered {
    uint32_t nid;
    std::string name;
    void (*fn)(ppu_context*);
};
static std::vector<Registered> registered;
extern "C" void ps3_hle_register_ctx(uint32_t nid, const char* name, void (*fn)(ppu_context*)) {
    registered.push_back({nid, name, fn});
}

// A stand-in microphone: a 440 Hz tone at 48 kHz stereo in 10 ms pushes from its own thread, or
// no device at all (VOICE_TEST_MIC=none).
static std::atomic<int> mic_opens{0}, mic_closes{0};
namespace {
class StandInMic final : public mag::voice::CaptureDevice {
public:
    ~StandInMic() override { close(); }
    Open open(mag::voice::CaptureBuffer& sink, const std::string&, std::string& error) override {
        const char* kind = std::getenv("VOICE_TEST_MIC");
        if (kind && std::strcmp(kind, "none") == 0) {
            error = "no capture device";
            return Open::Failed;
        }
        ++mic_opens;
        running_ = true;
        thread_ = std::thread([this, &sink] {
            double phase = 0;
            std::vector<float> chunk(2 * 480);
            while (running_) {
                for (size_t i = 0; i < 480; ++i) {
                    chunk[2 * i] = chunk[2 * i + 1] = float(0.4 * std::sin(phase));
                    phase += 2 * 3.14159265358979323846 * 440 / 48000;
                }
                sink.push(chunk.data(), 480, 48000, 2);
                std::this_thread::sleep_for(std::chrono::milliseconds(10));
            }
        });
        return Open::Opened;
    }
    void close() override {
        if (!thread_.joinable()) return;
        running_ = false;
        thread_.join();
        ++mic_closes;
    }

private:
    std::atomic<bool> running_{false};
    std::thread thread_;
};
}  // namespace
mag::voice::CaptureDeviceFactory mag::voice::host_capture_devices() {
    return [] { return std::unique_ptr<CaptureDevice>(new StandInMic()); };
}
#if MAG_VOICE_PUSH_TO_TALK
static std::atomic<int> ptt_held{0}, ptt_focused{1};
extern "C" int mag_game_controls_push_to_talk(int* held, int* focused) {
    *held = ptt_held;
    *focused = ptt_focused;
    return 1;
}
#endif

static int failures = 0;
#define CHECK(cond)                                                     \
    do {                                                                \
        if (!(cond)) {                                                  \
            std::printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); \
            ++failures;                                                 \
        }                                                               \
    } while (0)

static void put32(uint32_t a, uint32_t v) {
    vm_base[a] = uint8_t(v >> 24), vm_base[a + 1] = uint8_t(v >> 16), vm_base[a + 2] = uint8_t(v >> 8), vm_base[a + 3] = uint8_t(v);
}
static void put16(uint32_t a, uint16_t v) { vm_base[a] = uint8_t(v >> 8), vm_base[a + 1] = uint8_t(v); }
static void putf(uint32_t a, float v) {
    uint32_t bits;
    std::memcpy(&bits, &v, 4);
    put32(a, bits);
}
static uint32_t get32(uint32_t a) {
    return (uint32_t(vm_base[a]) << 24) | (uint32_t(vm_base[a + 1]) << 16) | (uint32_t(vm_base[a + 2]) << 8) | vm_base[a + 3];
}
static uint16_t get16(uint32_t a) { return uint16_t((vm_base[a] << 8) | vm_base[a + 1]); }

static uint32_t call(const char* name, uint64_t a = 0, uint64_t b = 0, uint64_t c = 0, double f = 0) {
    for (const auto& r : registered)
        if (r.name == name) {
            CHECK(r.nid == ps3_compute_nid(name));
            ppu_context ctx{};
            ctx.gpr[3] = a, ctx.gpr[4] = b, ctx.gpr[5] = c, ctx.fpr[1] = f;
            r.fn(&ctx);
            return static_cast<uint32_t>(ctx.gpr[3]);
        }
    std::printf("FAIL: %s not registered\n", name);
    ++failures;
    return 0xFFFFFFFFu;
}

// Port parameter block at `a`: type, threshold, mute, volume, bitrate.
static void params(uint32_t a, uint32_t type, uint32_t bitrate) {
    std::memset(vm_base + a, 0, 64);
    put32(a, type);
    put16(a + 4, 100);
    put16(a + 6, 0);
    putf(a + 8, 1.0f);
    put32(a + 12, bitrate);
}

int main(int argc, char** argv) {
    std::vector<uint8_t> memory(1 << 20);
    vm_base = memory.data();
    const std::string mode = argc > 1 ? argv[1] : "";
    // The settings window process and the extensions' prepare never register anything.
    if (auto prepare = mag_extension()->prepare) prepare("synthetic.elf");
    CHECK(registered.empty());
    if (auto start = mag_extension()->start) start("synthetic.elf");
    if (mode == "disabled") {
        CHECK(registered.empty());
    } else if (mode == "ptt" || mode == "receive") {
        CHECK(registered.size() == 20);
        const uint32_t init = 0x1000, param = 0x2000, id = 0x3000, size = 0x5000, data = 0x6000;
        put32(init, 0), put32(init + 4, 100), put32(init + 8, 1u << 29);
        CHECK(call("cellVoiceInit", init) == 0);
        params(param, 0, 0);
        CHECK(call("cellVoiceCreatePort", id, param) == 0);
        const uint32_t mic = get32(id);
        params(param, 4, 7300);
        CHECK(call("cellVoiceCreatePort", id, param) == 0);
        const uint32_t out = get32(id);
        CHECK(call("cellVoiceConnectIPortToOPort", mic, out) == 0);
        const auto wait = [](int ms) { std::this_thread::sleep_for(std::chrono::milliseconds(ms)); };
        uint8_t first = 0;
        const auto read = [&] {
            put32(size, 4096);
            CHECK(call("cellVoiceReadFromOPort", out, data, size) == 0);
            if (get32(size)) first = vm_base[data];
            return get32(size);
        };
        // The game reads the encoded port about once a frame; poll for `ms` and total what came.
        const auto poll = [&](int ms) {
            uint32_t total = 0;
            for (int t = 0; t < ms; t += 20) {
                std::this_thread::sleep_for(std::chrono::milliseconds(20));
                total += read();
            }
            return total;
        };
        wait(100);
        CHECK(mic_opens == 0);  // never before the game starts voice
        CHECK(call("cellVoiceStart") == 0);
        wait(150);
        CHECK(poll(200) == 0);  // open mic in the game, but push to talk is not held
        const bool device = mode == "ptt" && !(std::getenv("VOICE_TEST_MIC") && !std::strcmp(std::getenv("VOICE_TEST_MIC"), "none"));
        CHECK(mic_opens == (device ? 1 : 0));
#if MAG_VOICE_PUSH_TO_TALK
        ptt_held = 1;
#endif
        const uint32_t talking = poll(400);
        std::printf("held: %u bytes\n", talking);
        if (device) {
            CHECK(talking >= 6 * 37 && talking % 37 == 0 && first == 0xA1);
            float energy = 0;
            put32(size, 0);
            CHECK(call("cellVoiceGetSignalState", mic, 1000, size) == 0);
            uint32_t bits = get32(size);
            std::memcpy(&energy, &bits, 4);
            CHECK(energy > 0.05f);
        } else {
            CHECK(talking == 0);
        }
#if MAG_VOICE_PUSH_TO_TALK
        ptt_held = 0;
#endif
        const uint32_t tail = poll(400);
        std::printf("tail: %u bytes\n", tail);
        CHECK(tail <= 7 * 37);  // the 160 ms tail and a frame or two in flight
        CHECK(poll(300) == 0);
#if MAG_VOICE_PUSH_TO_TALK
        ptt_held = 1;
        ptt_focused = 0;  // another window has input focus
#endif
        CHECK(poll(300) == 0);
        CHECK(call("cellVoiceStop") == 0);
        CHECK(mic_closes == mic_opens);
        CHECK(call("cellVoiceEnd") == 0);
    } else {
        CHECK(registered.size() == 20);
        const uint32_t init = 0x1000, param = 0x2000, id = 0x3000, info = 0x4000, edges = 0x4100, size = 0x5000,
                       data = 0x6000;
        CHECK(call("cellVoiceInit", 0) == 0x80310805u);
        put32(init, 0), put32(init + 4, 100), put32(init + 8, 1u << 29);
        CHECK(call("cellVoiceInit", init) == 0);
        // The id goes to the first argument and the parameter block (second) is only read.
        params(param, 4, 7300);
        put32(id, 0xDEADBEEF);
        CHECK(call("cellVoiceCreatePort", id, param) == 0);
        CHECK(get32(id) != 0xDEADBEEF && get32(id) != 0);
        CHECK(get32(param) == 4 && get16(param + 4) == 100 && get32(param + 12) == 7300);
        CHECK(call("cellVoiceDeletePort", get32(id)) == 0);
        params(param, 0, 0);  // microphone
        CHECK(call("cellVoiceCreatePort", id, param) == 0);
        const uint32_t mic = get32(id);
        params(param, 4, 7300);  // encoded output
        CHECK(call("cellVoiceCreatePort", id, param) == 0);
        const uint32_t out = get32(id);
        params(param, 2, 7300);  // encoded input
        CHECK(call("cellVoiceCreatePort", id, param) == 0);
        const uint32_t in = get32(id);
        CHECK(mic != out && out != in);
        CHECK(call("cellVoiceConnectIPortToOPort", mic, out) == 0);
        CHECK(call("cellVoiceConnectIPortToOPort", out, mic) == 0x80310807u);
        // Info with room for four edges.
        std::memset(vm_base + info, 0, 64);
        put16(info + 8, 4);
        put32(info + 12, edges);
        CHECK(call("cellVoiceGetPortInfo", out, info) == 0);
        CHECK(get32(info) == 4 && get32(info + 4) == 0 && get16(info + 8) == 1 && get32(edges) == mic);
        CHECK(get32(info + 16) == 0 && get32(info + 20) == 37);
        CHECK(call("cellVoiceStart") == 0);
        std::this_thread::sleep_for(std::chrono::milliseconds(250));
        put32(size, 4096);
        CHECK(call("cellVoiceReadFromOPort", out, data, size) == 0);
        const uint32_t got = get32(size);
        CHECK(got >= 4 * 37 && got % 37 == 0);
        CHECK(vm_base[data] == 0xA1 && vm_base[data + 1] == 0);
        // Feed what was read into the encoded input, as a relay would deliver it.
        uint32_t pcm = 0;
        if (mode == "secondary") {
            params(param, 5, 0);  // the headset output
            CHECK(call("cellVoiceCreatePort", id, param) == 0);
            CHECK(call("cellVoiceConnectIPortToOPort", in, get32(id)) == 0);
        }
        if (mode == "tone") {
            // A 16-bit big-endian PCM port for the talker, as the game reads talkers.
            std::memset(vm_base + param, 0, 64);
            put32(param, 3);
            putf(param + 8, 1.0f);
            put32(param + 12, 3200);
            vm_base[param + 16] = 1;
            put32(param + 20, 2);
            put32(param + 24, 16000);
            CHECK(call("cellVoiceCreatePort", id, param) == 0);
            pcm = get32(id);
            CHECK(call("cellVoiceConnectIPortToOPort", in, pcm) == 0);
        }
        put32(size, got);
        CHECK(call("cellVoiceWriteToIPort", in, data, size) == 0 && get32(size) == got);
        if (mode == "secondary") std::this_thread::sleep_for(std::chrono::milliseconds(400));
        if (mode == "tone") {
            std::this_thread::sleep_for(std::chrono::milliseconds(120));
            std::memset(vm_base + info, 0, 64);
            CHECK(call("cellVoiceGetPortInfo", pcm, info) == 0 && get32(info + 20) == 1280 && get32(info + 16) > 0);
            put32(size, 8192);
            CHECK(call("cellVoiceReadFromOPort", pcm, data + 0x2000, size) == 0);
            const uint32_t pcm_bytes = get32(size);
            CHECK(pcm_bytes > 0 && pcm_bytes % 2 == 0);
            int loud = 0;
            for (uint32_t k = 0; k < pcm_bytes; k += 2) loud += std::abs(int16_t(get16(data + 0x2000 + k))) > 1000;
            std::printf("tone read back: %u bytes, %d loud samples\n", pcm_bytes, loud);
            CHECK(loud > 50);
        }
        put32(size, 0);
        CHECK(call("cellVoiceGetSignalState", mic, 1001, size) == 0 && get32(size) == 1);
        params(param, 2, 14400);
        put16(param + 6, 1);
        CHECK(call("cellVoiceUpdatePort", in, param) == 0);
        put32(id, 0xAAAAAAAA);
        CHECK(call("cellVoiceGetMuteFlag", in, id) == 0 && get16(id) == 1 && get16(id + 2) == 0xAAAA);  // 16-bit flag
        CHECK(call("cellVoiceSetVolume", in, 0, 0, 0.5) == 0);
        CHECK(call("cellVoiceGetVolume", in, id) == 0 && get32(id) == 0x3F000000u);
        CHECK(call("cellVoiceResetPort", 0x9999) == 0x80310807u);
        CHECK(call("cellVoiceStop") == 0);
        CHECK(call("cellVoiceEnd") == 0);
    }
    if (failures) {
        std::printf("%d failures\n", failures);
        return 1;
    }
    std::printf("PASS voice hle %s\n", mode.c_str());
    return 0;
}
