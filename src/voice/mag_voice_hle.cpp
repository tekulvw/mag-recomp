/* The game's voice-library calls, answered by the MAG voice service (src/voice/mag_voice_core.h).
 *
 * Inert unless switched on: a prepare callback reads the environment, and only when it selects a
 * voice mode are context handlers registered for the library's functions. The runtime dispatches
 * context handlers before its own table, so with voice off its cellVoice stub answers every call as
 * before and nothing here runs.
 *
 * MAG_VOICE_TEST_TONE=pattern  lab mode: no microphone; one second on, one second off, each frame a
 *                              block of counting bytes (docs/voice-chat.md)
 * MAG_VOICE_TEST_TONE=tone     lab mode: the same cadence carrying an Opus-coded 440 Hz tone; received
 *                              talkers are decoded and played on a host stream (the game's headset
 *                              output; PS3_MUTE keeps it silent)
 * MAG_VOICE_VOLUME=<0..200>    voice playback volume in percent (default 100)
 * MAG_VOICE_TRACE=<path>       JSON lines of the calls: sizes, counts and times, never audio */
#include "mag_voice_core.h"
#include "mag_voice_opus.h"
#include "mag_voice_output.h"
#include "mag_extension.h"
#include "ppu/ppu_context.h"
#include "ps3emu/nid.h"

#include <chrono>
#include <cstdarg>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <string>

extern "C" uint8_t* vm_base;
extern "C" void ps3_hle_register_ctx(uint32_t nid, const char* name, void (*fn)(ppu_context*));

namespace {
using namespace mag::voice;

std::unique_ptr<Service> g_service;
// The host stream that plays the headset output, open between the game's start and stop.
bool g_plays = false;
std::unique_ptr<VoiceOutput> g_output;

void open_output() {
    if (!g_plays || g_output) return;
    std::string description;
    bool on_device = false;
    g_output = open_voice_output([](int16_t* out, size_t count) { g_service->render_secondary(out, count); },
                                 std::getenv("PS3_MUTE"), description, on_device);
    std::fprintf(stderr, "[voice] talkers play through %s\n", description.c_str());
    if (g_service->tracing()) g_service->trace_event("output", on_device ? "\"device\":1" : "\"device\":0");
}

// Guest memory is big-endian.
uint8_t* guest(uint32_t address) { return vm_base + address; }
uint32_t load32(uint32_t a) {
    const uint8_t* p = guest(a);
    return (uint32_t(p[0]) << 24) | (uint32_t(p[1]) << 16) | (uint32_t(p[2]) << 8) | uint32_t(p[3]);
}
uint16_t load16(uint32_t a) {
    const uint8_t* p = guest(a);
    return static_cast<uint16_t>((p[0] << 8) | p[1]);
}
float loadf(uint32_t a) {
    const uint32_t bits = load32(a);
    float value;
    std::memcpy(&value, &bits, sizeof value);
    return value;
}
void store32(uint32_t a, uint32_t v) {
    uint8_t* p = guest(a);
    p[0] = uint8_t(v >> 24), p[1] = uint8_t(v >> 16), p[2] = uint8_t(v >> 8), p[3] = uint8_t(v);
}
void store16(uint32_t a, uint16_t v) {
    uint8_t* p = guest(a);
    p[0] = uint8_t(v >> 8), p[1] = uint8_t(v);
}
void storef(uint32_t a, float v) {
    uint32_t bits;
    std::memcpy(&bits, &v, sizeof bits);
    store32(a, bits);
}

uint32_t arg(ppu_context* c, int n) { return static_cast<uint32_t>(c->gpr[3 + n]); }
void ret(ppu_context* c, uint32_t status) { c->gpr[3] = status; }

// The public port parameter block: type, threshold, mute flag, volume, then the bitrate (voice
// ports) or the PCM buffer size and format (PCM ports).
PortParams read_params(uint32_t a) {
    PortParams p;
    p.type = load32(a);
    p.threshold = load16(a + 4);
    p.mute = load16(a + 6);
    p.volume = loadf(a + 8);
    p.bitrate = load32(a + 12);
    p.pcm_buffer = load32(a + 12);
    p.pcm_channels = guest(a + 16)[0];
    p.pcm_alignment = guest(a + 17)[0];
    p.pcm_type = load32(a + 20);
    p.pcm_rate = load32(a + 24);
    return p;
}

#if defined(__GNUC__) || defined(__clang__)
void trace(const char* name, const char* format, ...) __attribute__((format(printf, 2, 3)));
#endif
void trace(const char* name, const char* format, ...) {
    if (!g_service->tracing()) return;
    char fields[512];
    va_list list;
    va_start(list, format);
    std::vsnprintf(fields, sizeof fields, format, list);
    va_end(list);
    g_service->trace_event(name, fields);
}

void voice_init(ppu_context* c) {
    const uint32_t param = arg(c, 0);
    if (!param) return ret(c, status::ArgumentInvalid);
    const uint32_t r = g_service->init();
    trace("init", "\"event_mask\":%u,\"version\":%u,\"app_type\":%u,\"status\":%u", load32(param), load32(param + 4),
          load32(param + 8), r);
    ret(c, r);
}
void voice_end(ppu_context* c) {
    g_output.reset();  // stops pulling before the ports go
    const uint32_t r = g_service->end();
    trace("end", "\"status\":%u", r);
    ret(c, r);
}
void voice_start(ppu_context* c) {
    const uint32_t r = g_service->start();
    trace("start", "\"status\":%u", r);
    if (r == status::Ok) open_output();
    ret(c, r);
}
void voice_start_ex(ppu_context* c) {
    if (!arg(c, 0)) return ret(c, status::ArgumentInvalid);
    voice_start(c);
}
void voice_stop(ppu_context* c) {
    g_output.reset();
    const uint32_t r = g_service->stop();
    trace("stop", "\"status\":%u", r);
    ret(c, r);
}
// The public signatures put the port-id out-pointer first: CreatePort(portId*, param*).
void voice_create_port(ppu_context* c) {
    const uint32_t out = arg(c, 0), param = arg(c, 1);
    if (!param || !out) return ret(c, status::ArgumentInvalid);
    const PortParams p = read_params(param);
    uint32_t id = 0;
    const uint32_t r = g_service->create_port(p, id);
    if (r == status::Ok) store32(out, id);
    char fields[384];
    describe_params(fields, sizeof fields, p);
    trace("create_port", "%s,\"port\":%u,\"status\":%u", fields, id, r);
    ret(c, r);
}
void voice_delete_port(ppu_context* c) {
    const uint32_t r = g_service->delete_port(arg(c, 0));
    trace("delete_port", "\"port\":%u,\"status\":%u", arg(c, 0), r);
    ret(c, r);
}
void voice_connect(ppu_context* c) {
    const uint32_t r = g_service->connect(arg(c, 0), arg(c, 1));
    trace("connect", "\"input\":%u,\"output\":%u,\"status\":%u", arg(c, 0), arg(c, 1), r);
    ret(c, r);
}
void voice_disconnect(ppu_context* c) {
    const uint32_t r = g_service->disconnect(arg(c, 0), arg(c, 1));
    trace("disconnect", "\"input\":%u,\"output\":%u,\"status\":%u", arg(c, 0), arg(c, 1), r);
    ret(c, r);
}
void voice_reset_port(ppu_context* c) {
    const uint32_t r = g_service->reset_port(arg(c, 0));
    trace("reset_port", "\"port\":%u,\"status\":%u", arg(c, 0), r);
    ret(c, r);
}
void voice_update_port(ppu_context* c) {
    const uint32_t param = arg(c, 1);
    if (!param) return ret(c, status::ArgumentInvalid);
    const PortParams p = read_params(param);
    const uint32_t r = g_service->update_port(arg(c, 0), p);
    trace("update_port", "\"port\":%u,\"threshold\":%u,\"mute\":%u,\"volume\":%.4f,\"bitrate\":%u,\"status\":%u",
          arg(c, 0), p.threshold, p.mute, static_cast<double>(p.volume), p.bitrate, r);
    ret(c, r);
}
void voice_set_mute(ppu_context* c) {
    const uint32_t r = g_service->set_mute(arg(c, 0), arg(c, 1) != 0);
    trace("set_mute", "\"port\":%u,\"mute\":%u,\"status\":%u", arg(c, 0), arg(c, 1), r);
    ret(c, r);
}
void voice_get_mute(ppu_context* c) {
    bool muted = false;
    const uint32_t r = g_service->mute(arg(c, 0), muted);
    if (r == status::Ok && arg(c, 1)) store16(arg(c, 1), muted ? 1 : 0);  // a 16-bit flag
    ret(c, r);
}
void voice_set_volume(ppu_context* c) {
    const float volume = static_cast<float>(c->fpr[1]);
    const uint32_t r = g_service->set_volume(arg(c, 0), volume);
    trace("set_volume", "\"port\":%u,\"volume\":%.4f,\"status\":%u", arg(c, 0), static_cast<double>(volume), r);
    ret(c, r);
}
void voice_get_volume(ppu_context* c) {
    float volume = 0;
    const uint32_t r = g_service->volume(arg(c, 0), volume);
    if (r == status::Ok && arg(c, 1)) storef(arg(c, 1), volume);
    ret(c, r);
}
// The public port info block: type, state, edge count (u16), edge array pointer, byte count,
// frame size. Edges are written only when the caller supplied an array and room for them.
void voice_get_port_info(ppu_context* c) {
    const uint32_t out = arg(c, 1);
    if (!out) return ret(c, status::ArgumentInvalid);
    PortInfo info;
    const uint32_t r = g_service->port_info(arg(c, 0), info);
    const uint16_t room = load16(out + 8);
    const uint32_t edges = load32(out + 12);
    if (r == status::Ok) {
        store32(out, info.type);
        store32(out + 4, info.state);
        store16(out + 8, static_cast<uint16_t>(info.edges.size()));
        if (edges && room >= info.edges.size())
            for (size_t i = 0; i < info.edges.size(); ++i) store32(edges + uint32_t(4 * i), info.edges[i]);
        store32(out + 16, info.bytes);
        store32(out + 20, info.frame_bytes);
    }
    trace("port_info", "\"port\":%u,\"edge_room\":%u,\"edge_array\":%u,\"edges\":%zu,\"bytes\":%u,\"frame\":%u,\"status\":%u",
          arg(c, 0), room, edges ? 1u : 0u, info.edges.size(), info.bytes, info.frame_bytes, r);
    ret(c, r);
}
void voice_read(ppu_context* c) {
    const uint32_t data = arg(c, 1), size = arg(c, 2);
    if (!size) return ret(c, status::ArgumentInvalid);
    const uint32_t asked = load32(size);
    size_t copied = 0;
    const uint32_t r = g_service->read(arg(c, 0), data ? guest(data) : nullptr, data ? asked : 0, copied);
    store32(size, static_cast<uint32_t>(copied));
    trace("read", "\"port\":%u,\"asked\":%u,\"copied\":%zu,\"status\":%u", arg(c, 0), asked, copied, r);
    ret(c, r);
}
void voice_write(ppu_context* c) {
    const uint32_t data = arg(c, 1), size = arg(c, 2);
    if (!size) return ret(c, status::ArgumentInvalid);
    const uint32_t offered = load32(size);
    size_t accepted = 0;
    const uint32_t r = g_service->write(arg(c, 0), data ? guest(data) : nullptr, data ? offered : 0, accepted);
    store32(size, static_cast<uint32_t>(accepted));
    trace("write", "\"port\":%u,\"offered\":%u,\"accepted\":%zu,\"status\":%u", arg(c, 0), offered, accepted, r);
    ret(c, r);
}
void voice_signal_state(ppu_context* c) {
    const uint32_t attr = arg(c, 1), out = arg(c, 2);
    float energy = 0;
    uint32_t vad = 0;
    bool is_float = false;
    const uint32_t r = g_service->signal_state(arg(c, 0), attr, energy, vad, is_float);
    if (r == status::Ok && out) {
        if (is_float) storef(out, energy); else store32(out, vad);
    }
    trace("signal_state", "\"port\":%u,\"attr\":%u,\"status\":%u", arg(c, 0), attr, r);
    ret(c, r);
}

uint64_t monotonic_ms() {
    using namespace std::chrono;
    return static_cast<uint64_t>(duration_cast<milliseconds>(steady_clock::now().time_since_epoch()).count());
}

void voice_prepare(const char*) {
    const char* tone = std::getenv("MAG_VOICE_TEST_TONE");
    const char* mode = std::getenv("MAG_VOICE_MODE");
    const bool pattern = tone && std::strcmp(tone, "pattern") == 0;
    const bool coded = tone && std::strcmp(tone, "tone") == 0;
    if (!pattern && !coded) {
        if ((tone && *tone && std::strcmp(tone, "0") != 0) || (mode && *mode && std::strcmp(mode, "off") != 0))
            std::fprintf(stderr, "[voice] this build has only the lab modes (MAG_VOICE_TEST_TONE=pattern or tone); "
                                 "voice stays off\n");
        return;
    }
    ServiceOptions options;
    options.now_ms = monotonic_ms;
    options.source = std::make_unique<TestToneSource>();
    if (coded) {
        options.encoder = make_opus_encoder();
        options.decoder = opus_decoder_factory();
        g_plays = true;
    } else {
        options.encoder = std::make_unique<PatternEncoder>();
    }
    if (const char* volume = std::getenv("MAG_VOICE_VOLUME")) {
        char* end = nullptr;
        const long percent = std::strtol(volume, &end, 10);
        if (end != volume && *end == 0 && percent >= 0 && percent <= 200) options.voice_gain = float(percent) / 100.0f;
        else std::fprintf(stderr, "[voice] MAG_VOICE_VOLUME=%s is not 0..200; using 100\n", volume);
    }
    options.trace = Trace::open(std::getenv("MAG_VOICE_TRACE"));
    g_service = std::make_unique<Service>(std::move(options));
    struct Entry { const char* name; void (*fn)(ppu_context*); };
    static const Entry entries[] = {
        {"cellVoiceInit", voice_init}, {"cellVoiceInitEx", voice_init}, {"cellVoiceEnd", voice_end},
        {"cellVoiceStart", voice_start}, {"cellVoiceStartEx", voice_start_ex}, {"cellVoiceStop", voice_stop},
        {"cellVoiceCreatePort", voice_create_port}, {"cellVoiceDeletePort", voice_delete_port},
        {"cellVoiceConnectIPortToOPort", voice_connect}, {"cellVoiceDisconnectIPortFromOPort", voice_disconnect},
        {"cellVoiceResetPort", voice_reset_port}, {"cellVoiceUpdatePort", voice_update_port},
        {"cellVoiceSetMuteFlag", voice_set_mute}, {"cellVoiceGetMuteFlag", voice_get_mute},
        {"cellVoiceSetVolume", voice_set_volume}, {"cellVoiceGetVolume", voice_get_volume},
        {"cellVoiceGetPortInfo", voice_get_port_info}, {"cellVoiceReadFromOPort", voice_read},
        {"cellVoiceWriteToIPort", voice_write}, {"cellVoiceGetSignalState", voice_signal_state}};
    for (const auto& entry : entries) ps3_hle_register_ctx(ps3_compute_nid(entry.name), entry.name, entry.fn);
    std::fprintf(stderr, "[voice] lab %s mode: %zu voice-library handlers registered\n", coded ? "tone" : "pattern",
                 sizeof entries / sizeof entries[0]);
}

struct Registration {
    Registration() {
        MagExtensionCallbacks callbacks{};
        callbacks.prepare = voice_prepare;
        if (mag_extension_register(&callbacks) != 0) std::fprintf(stderr, "[voice] could not register its prepare callback\n");
    }
} registration;
}  // namespace
