// Synthetic checks for the MAG voice service core (src/voice/mag_voice_core.h). No game data.
#include "mag_voice_core.h"

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>

using namespace mag::voice;

static int failures = 0;
#define CHECK(cond)                                                              \
    do {                                                                         \
        if (!(cond)) {                                                           \
            std::printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond);          \
            ++failures;                                                          \
        }                                                                        \
    } while (0)

static std::vector<uint8_t> pattern_stream(size_t block, uint64_t first, size_t count) {
    std::vector<uint8_t> stream(block * count);
    PatternEncoder encoder;
    std::vector<uint8_t> payload(block - BlockHeader);
    for (size_t i = 0; i < count; ++i) {
        const size_t n = encoder.encode(first + i, nullptr, payload.data(), payload.size());
        CHECK(write_block(static_cast<uint8_t>(first + i), payload.data(), n, block, stream.data() + i * block) == block);
    }
    return stream;
}

static void block_sizes() {
    CHECK(block_size_for_bitrate(7300) == 37);  // 36.5 bytes per 40 ms, rounded up
    CHECK(block_size_for_bitrate(3850) == 20);
    CHECK(block_size_for_bitrate(14400) == 72);
    CHECK(block_size_for_bitrate(0) == 8);
    uint8_t out[16];
    uint8_t payload[16] = {};
    CHECK(write_block(0, payload, 13, 16, out) == 0);  // payload larger than the block allows
    CHECK(write_block(0, payload, 12, 16, out) == 16);
}

static void parser_every_split() {
    const size_t block = 37;
    const auto stream = pattern_stream(block, 250, 12);  // sequence wraps past 255
    for (size_t chunk = 1; chunk <= stream.size(); ++chunk) {
        BlockParser parser(block);
        size_t blocks = 0;
        uint8_t expected = 250;
        Block out;
        for (size_t at = 0; at < stream.size(); at += chunk) {
            parser.push(stream.data() + at, std::min(chunk, stream.size() - at));
            while (parser.next(out)) {
                CHECK(out.sequence == expected);
                CHECK(pattern_payload_valid(out.payload));
                ++expected;
                ++blocks;
            }
        }
        CHECK(blocks == 12);
        CHECK(parser.skipped_bytes() == 0);
        CHECK(parser.pending() == 0);
    }
}

static void parser_recovers() {
    const size_t block = 37;
    auto stream = pattern_stream(block, 0, 6);
    // Lose 5 bytes from the middle of block 1 and corrupt block 3's padding check by a stray byte.
    stream.erase(stream.begin() + 40, stream.begin() + 45);
    std::vector<uint8_t> noisy = {0x00, 0xA1, 0xFF, 0x12};  // junk, including a marker with a bad length
    stream.insert(stream.begin(), noisy.begin(), noisy.end());
    BlockParser parser(block);
    parser.push(stream.data(), stream.size());
    Block out;
    std::vector<uint8_t> sequences;
    while (parser.next(out)) {
        CHECK(pattern_payload_valid(out.payload));
        sequences.push_back(out.sequence);
    }
    // Blocks 0, 2, 3, 4 and 5 survive; the damaged block 1 never decodes as a valid block.
    CHECK((sequences == std::vector<uint8_t>{0, 2, 3, 4, 5}));
    CHECK(parser.skipped_bytes() > 0);
}

static void test_tone() {
    TestToneSource source;
    std::vector<int16_t> samples(FrameSamples);
    CHECK(source.frame(0, samples.data()));
    bool loud = false;
    for (int16_t s : samples) loud |= s > 4000;
    CHECK(loud);
    CHECK(source.frame(24, samples.data()));
    CHECK(!source.frame(25, samples.data()));
    for (int16_t s : samples) CHECK(s == 0);
    CHECK(source.frame(50, samples.data()));
}

struct Clock {
    uint64_t ms = 1000;
};

static Service make(Clock& clock, bool with_source = true) {
    ServiceOptions options;
    options.now_ms = [&clock] { return clock.ms; };
    if (with_source) options.source = std::make_unique<TestToneSource>();
    options.encoder = std::make_unique<PatternEncoder>();
    return Service(std::move(options));
}

static PortParams voice_port(PortType type) {
    PortParams p;
    p.type = uint32_t(type);
    p.bitrate = 7300;
    return p;
}

static void many_ports() {
    Clock clock;
    Service service = make(clock, false);
    service.init();
    uint32_t id = 0;
    for (int i = 0; i < 40; ++i) CHECK(service.create_port(voice_port(i % 2 ? PortType::InVoice : PortType::OutPcm), id) == status::Ok);
    CHECK(id <= 0xFFFFu);
}

static void lifecycle_and_topology() {
    Clock clock;
    Service service = make(clock);
    uint32_t id = 0;
    CHECK(service.create_port(voice_port(PortType::OutVoice), id) == status::NotInitialized);
    CHECK(service.init() == status::Ok);
    CHECK(service.init() == status::AlreadyInitialized);
    uint32_t mic = 0, out = 0, in = 0, head = 0;
    PortParams mic_params;
    mic_params.type = uint32_t(PortType::InMic);
    CHECK(service.create_port(mic_params, mic) == status::Ok);
    CHECK(service.create_port(voice_port(PortType::OutVoice), out) == status::Ok);
    CHECK(service.create_port(voice_port(PortType::InVoice), in) == status::Ok);
    PortParams head_params;
    head_params.type = uint32_t(PortType::OutSecondary);
    CHECK(service.create_port(head_params, head) == status::Ok);
    CHECK(mic != 0 && out != 0 && in != 0 && head != 0 && mic != out);
    PortParams bad;
    bad.type = 9;
    CHECK(service.create_port(bad, id) == status::ArgumentInvalid);
    // Inputs are types 0..2, outputs 3..5.
    CHECK(service.connect(out, mic) == status::Topology);
    CHECK(service.connect(mic, in) == status::Topology);
    CHECK(service.connect(mic, out) == status::Ok);
    CHECK(service.connect(mic, out) == status::Ok);  // a repeat adds no second edge
    CHECK(service.connect(in, head) == status::Ok);
    PortInfo info;
    CHECK(service.port_info(out, info) == status::Ok);
    CHECK(info.type == uint32_t(PortType::OutVoice));
    CHECK(info.state == uint32_t(PortState::Idle));
    CHECK(info.edges == std::vector<uint32_t>{mic});
    CHECK(info.frame_bytes == 37);
    CHECK(info.bytes == 0);
    CHECK(service.port_info(0x7777, info) == status::Topology);
    CHECK(service.disconnect(mic, out) == status::Ok);
    CHECK(service.port_info(out, info) == status::Ok && info.edges.empty());
    CHECK(service.delete_port(in) == status::Ok);
    CHECK(service.port_info(head, info) == status::Ok && info.edges.empty());
    CHECK(service.delete_port(in) == status::Topology);
    bool muted = true;
    CHECK(service.set_mute(out, true) == status::Ok && service.mute(out, muted) == status::Ok && muted);
    float volume = 0;
    CHECK(service.set_volume(out, 0.25f) == status::Ok && service.volume(out, volume) == status::Ok && volume == 0.25f);
    CHECK(service.end() == status::Ok);
    CHECK(service.end() == status::NotInitialized);
}

static void encoded_output_follows_gate() {
    Clock clock;
    Service service = make(clock);
    service.init();
    uint32_t mic = 0, out = 0;
    PortParams mic_params;
    mic_params.type = uint32_t(PortType::InMic);
    service.create_port(mic_params, mic);
    service.create_port(voice_port(PortType::OutVoice), out);
    service.connect(mic, out);
    uint8_t buffer[1024];
    size_t copied = 1;
    // Nothing before the service starts.
    clock.ms += 400;
    CHECK(service.read(out, buffer, sizeof buffer, copied) == status::Ok && copied == 0);
    service.start();
    clock.ms += 200;  // five frames
    PortInfo info;
    CHECK(service.port_info(out, info) == status::Ok && info.bytes == 5 * 37 && info.state == uint32_t(PortState::Running));
    // Whole blocks only: room for two and a half blocks returns two.
    CHECK(service.read(out, buffer, 37 * 2 + 18, copied) == status::Ok && copied == 74);
    CHECK(service.read(out, buffer, 10, copied) == status::Ok && copied == 0);
    CHECK(service.read(out, buffer, sizeof buffer, copied) == status::Ok && copied == 3 * 37);
    BlockParser parser(37);
    parser.push(buffer, copied);
    Block block;
    CHECK(parser.next(block) && block.sequence == 2 && pattern_payload_valid(block.payload));
    // The second half of each two-second cycle is silent: no blocks.
    clock.ms += 1000;  // frames 5..29: 20 open, 5 closed
    service.read(out, buffer, sizeof buffer, copied);
    clock.ms += 800;   // frames 30..49, all closed
    CHECK(service.read(out, buffer, sizeof buffer, copied) == status::Ok && copied == 0);
    float energy = 1;
    uint32_t vad = 1;
    bool is_float = false;
    CHECK(service.signal_state(mic, AttrVad, energy, vad, is_float) == status::Ok && vad == 0 && !is_float);
    clock.ms += 120;   // frames 50..52, open again
    CHECK(service.signal_state(mic, AttrVad, energy, vad, is_float) == status::Ok && vad == 1);
    CHECK(service.signal_state(mic, AttrEnergyLevel, energy, vad, is_float) == status::Ok && is_float && energy > 0.1f);
    CHECK(service.signal_state(mic, 1005, energy, vad, is_float) == status::DeviceNotPresent);
    // A muted microphone produces nothing.
    service.read(out, buffer, sizeof buffer, copied);
    service.set_mute(mic, true);
    clock.ms += 200;
    CHECK(service.read(out, buffer, sizeof buffer, copied) == status::Ok && copied == 0);
    service.set_mute(mic, false);
    // After a long stall at most max_catch_up frames are produced and the queue stays bounded.
    clock.ms += 60000;
    CHECK(service.port_info(out, info) == status::Ok && info.bytes <= 12 * 37);
    // Stop halts production; reset clears the queue.
    service.stop();
    service.reset_port(out);
    clock.ms += 400;
    CHECK(service.port_info(out, info) == status::Ok && info.bytes == 0);
    const SendStats sent = service.send_stats();
    CHECK(sent.blocks > 0 && sent.frames > 0);
}

static void encoded_input_counts_blocks() {
    Clock clock;
    Service service = make(clock, false);
    service.init();
    uint32_t in = 0;
    service.create_port(voice_port(PortType::InVoice), in);
    const auto stream = pattern_stream(37, 7, 10);
    size_t accepted = 0;
    // Written in uneven pieces, as a network path might deliver them.
    size_t at = 0;
    for (size_t piece : {5u, 60u, 1u, 100u, 204u}) {
        CHECK(service.write(in, stream.data() + at, piece, accepted) == status::Ok && accepted == piece);
        at += piece;
    }
    CHECK(at == stream.size());
    ReceiveStats got = service.receive_stats();
    CHECK(got.blocks == 10 && got.pattern_ok == 10 && got.pattern_bad == 0 && got.sequence_gaps == 0);
    // A gap in the sequence is counted; a reset forgets the last sequence.
    const auto later = pattern_stream(37, 30, 1);
    service.write(in, later.data(), later.size(), accepted);
    CHECK(service.receive_stats().sequence_gaps == 1);
    service.reset_port(in);
    service.write(in, stream.data(), 37, accepted);
    CHECK(service.receive_stats().sequence_gaps == 1);
    // Bounded: junk beyond the port's capacity is refused, not buffered.
    std::vector<uint8_t> junk(10000, 0x55);
    CHECK(service.write(in, junk.data(), junk.size(), accepted) == status::Ok && accepted <= 4096);
    // Output types cannot be written, input types cannot be read.
    uint32_t out = 0;
    service.create_port(voice_port(PortType::OutVoice), out);
    CHECK(service.write(out, junk.data(), 4, accepted) == status::Topology);
    size_t copied = 0;
    CHECK(service.read(in, junk.data(), 4, copied) == status::Topology);
}

static void update_port_rules() {
    Clock clock;
    Service service = make(clock, false);
    service.init();
    uint32_t in = 0;
    service.create_port(voice_port(PortType::InVoice), in);
    PortParams change = voice_port(PortType::OutPcm);  // the type never changes
    change.bitrate = 14400;
    change.mute = 1;
    change.volume = 0.5f;
    CHECK(service.update_port(in, change) == status::Ok);
    PortInfo info;
    CHECK(service.port_info(in, info) == status::Ok && info.type == uint32_t(PortType::InVoice) && info.frame_bytes == 72);
    CHECK(info.bytes == 4096);  // an input port reports the bytes it can take
    std::vector<uint8_t> partial(50, 0x11);
    size_t accepted = 0;
    service.write(in, partial.data(), partial.size(), accepted);
    CHECK(service.port_info(in, info) == status::Ok && info.bytes < 4096);
    bool muted = false;
    CHECK(service.mute(in, muted) == status::Ok && muted);
}

static void trace_has_no_payload() {
    const char* path = std::getenv("VOICE_TRACE_PATH");
    if (!path) return;
    Clock clock;
    ServiceOptions options;
    options.now_ms = [&clock] { return clock.ms; };
    options.source = std::make_unique<TestToneSource>();
    options.encoder = std::make_unique<PatternEncoder>();
    options.trace = Trace::open(path);
    Service service(std::move(options));
    service.init();
    service.trace_event("probe", "\"port\":1");
    service.start();
    clock.ms += 6000;
    PortInfo info;
    service.port_info(1, info);
    service.end();
}

int main() {
    block_sizes();
    parser_every_split();
    parser_recovers();
    test_tone();
    many_ports();
    lifecycle_and_topology();
    encoded_output_follows_gate();
    encoded_input_counts_blocks();
    update_port_rules();
    trace_has_no_payload();
    if (failures) {
        std::printf("%d failures\n", failures);
        return 1;
    }
    std::printf("PASS voice core\n");
    return 0;
}
