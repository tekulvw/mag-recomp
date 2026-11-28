// Synthetic checks for the MAG voice service core (src/voice/mag_voice_core.h). No game data.
#include "mag_voice_core.h"

#include <algorithm>
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


// A stand-in decoder: a block decodes to a constant frame of its first payload byte times 100;
// concealment yields -7.
class FakeDecoder final : public FrameDecoder {
public:
    bool decode(const uint8_t* data, size_t bytes, int16_t* samples) override {
        if (data && bytes == 1 && data[0] == 0xEE) return false;  // undecodable
        const int16_t value = data ? int16_t(data[0] * 100) : int16_t(-7);
        for (unsigned i = 0; i < FrameSamples; ++i) samples[i] = value;
        return true;
    }
};

static std::vector<uint8_t> one_block(uint8_t sequence, uint8_t value) {
    std::vector<uint8_t> block(37);
    write_block(sequence, &value, 1, 37, block.data());
    return block;
}

static int16_t be16(const uint8_t* p) { return int16_t((p[0] << 8) | p[1]); }

static void playout_to_pcm_port() {
    Clock clock;
    ServiceOptions options;
    options.now_ms = [&clock] { return clock.ms; };
    options.decoder = [] { return std::unique_ptr<FrameDecoder>(std::make_unique<FakeDecoder>()); };
    options.voice_gain = 1.0f;
    Service service(std::move(options));
    service.init();
    uint32_t in = 0, pcm = 0, stereo = 0;
    service.create_port(voice_port(PortType::InVoice), in);
    PortParams pcm_params;
    pcm_params.type = uint32_t(PortType::OutPcm);
    pcm_params.pcm_type = 2;  // 16-bit big-endian
    pcm_params.pcm_channels = 1;
    pcm_params.pcm_buffer = 3200;
    service.create_port(pcm_params, pcm);
    PortParams stereo_params = pcm_params;
    stereo_params.pcm_type = 1;  // float little-endian
    stereo_params.pcm_channels = 2;
    service.create_port(stereo_params, stereo);
    CHECK(service.connect(in, pcm) == status::Ok);
    service.start();
    PortInfo info;
    CHECK(service.port_info(pcm, info) == status::Ok && info.frame_bytes == 640 * 2 && info.bytes == 0);
    size_t accepted = 0, copied = 0;
    std::vector<uint8_t> out(8192);
    // One block (40 ms) is less than the 60 ms hold: nothing plays yet.
    auto b0 = one_block(0, 1);
    service.write(in, b0.data(), b0.size(), accepted);
    clock.ms += 20;
    CHECK(service.read(pcm, out.data(), out.size(), copied) == status::Ok && copied == 0);
    // A second block reaches the hold; playout starts as it arrives and releases in real time.
    auto b1 = one_block(1, 2);
    service.write(in, b1.data(), b1.size(), accepted);
    clock.ms += 1;
    CHECK(service.read(pcm, out.data(), out.size(), copied) == status::Ok && copied == 32);
    clock.ms += 20;  // 20 ms more due: 320 samples
    CHECK(service.port_info(pcm, info) == status::Ok && info.bytes == 320 * 2);
    CHECK(service.read(pcm, out.data(), out.size(), copied) == status::Ok && copied == 640);
    CHECK(be16(out.data()) == 100 && be16(out.data() + 638) == 100);
    // Whole samples only, up to the capacity.
    clock.ms += 10;
    CHECK(service.read(pcm, out.data(), 101, copied) == status::Ok && copied == 100);
    // A gap of two blocks is concealed (two frames of -7) before block 4 plays.
    auto b4 = one_block(4, 3);
    service.write(in, b4.data(), b4.size(), accepted);
    ReceiveStats got = service.receive_stats();
    CHECK(got.frames_concealed == 2 && got.sequence_gaps == 1 && got.frames_decoded == 3);
    // An undecodable payload is counted and skipped.
    std::vector<uint8_t> bad(37);
    uint8_t ee = 0xEE;
    write_block(5, &ee, 1, 37, bad.data());
    service.write(in, bad.data(), bad.size(), accepted);
    CHECK(service.receive_stats().decode_errors == 1);
    // Play everything out: 4 frames of audio plus the two concealed, minus what was read.
    clock.ms += 1000;
    size_t total = 0;
    int concealed = 0;
    while (service.read(pcm, out.data(), out.size(), copied) == status::Ok && copied) {
        for (size_t i = 0; i < copied; i += 2) concealed += be16(out.data() + i) == -7;
        total += copied / 2;
    }
    CHECK(total == 5 * 640 - 16 - 320 - 50);
    CHECK(concealed == 2 * 640);
    CHECK(service.receive_stats().underruns >= 1);
    // Mute and volume: the input's volume and the output's mute apply.
    service.reset_port(in);
    service.set_volume(in, 0.5f);
    auto c0 = one_block(9, 4), c1 = one_block(10, 4);
    service.write(in, c0.data(), c0.size(), accepted);
    service.write(in, c1.data(), c1.size(), accepted);
    clock.ms += 10;
    CHECK(service.read(pcm, out.data(), out.size(), copied) == status::Ok && copied == 320);
    CHECK(be16(out.data()) == 200);
    service.set_mute(pcm, true);
    clock.ms += 10;
    CHECK(service.read(pcm, out.data(), out.size(), copied) == status::Ok && copied == 320 && be16(out.data()) == 0);
    service.set_mute(pcm, false);
    // A float stereo output gets the same audio on both channels, little-endian.
    service.disconnect(in, pcm);
    service.connect(in, stereo);
    clock.ms += 10;
    CHECK(service.read(stereo, out.data(), out.size(), copied) == status::Ok && copied == 160 * 8);
    float left, right;
    std::memcpy(&left, out.data(), 4);
    std::memcpy(&right, out.data() + 4, 4);
    CHECK(left == right && left > 0.0060f && left < 0.0062f);  // 400 / 32768 * 0.5
    // Reset forgets a talker's audio.
    service.reset_port(in);
    clock.ms += 100;
    CHECK(service.read(stereo, out.data(), out.size(), copied) == status::Ok && copied == 0);
    // Nobody reading: a talker's ready audio stays bounded.
    for (uint8_t i = 0; i < 40; ++i) {
        auto b = one_block(i, 1);
        service.write(in, b.data(), b.size(), accepted);
        clock.ms += 40;
    }
    clock.ms += 2000;
    CHECK(service.port_info(stereo, info) == status::Ok && info.bytes <= 300 * 16 * 8);
}

// The game connects received talkers to the headset output (OUT_SECONDARY) and reads no PCM port:
// a host stream pulls them through render_secondary.
static void playout_to_secondary() {
    Clock clock;
    ServiceOptions options;
    options.now_ms = [&clock] { return clock.ms; };
    options.decoder = [] { return std::unique_ptr<FrameDecoder>(std::make_unique<FakeDecoder>()); };
    options.voice_gain = 0.5f;
    Service service(std::move(options));
    service.init();
    uint32_t a = 0, b = 0, headset = 0, other = 0, static_in = 0;
    service.create_port(voice_port(PortType::InVoice), a);
    service.create_port(voice_port(PortType::InVoice), b);
    PortParams secondary;
    secondary.type = uint32_t(PortType::OutSecondary);
    secondary.volume = 1.0f;
    service.create_port(secondary, headset);
    service.create_port(secondary, other);
    PortParams pcm_in;
    pcm_in.type = uint32_t(PortType::InPcm);
    pcm_in.pcm_type = 2;  // 16-bit big-endian
    pcm_in.pcm_channels = 1;
    pcm_in.pcm_buffer = 8192;
    service.create_port(pcm_in, static_in);
    CHECK(service.connect(a, headset) == status::Ok);
    CHECK(service.connect(b, headset) == status::Ok);
    CHECK(service.connect(a, other) == status::Ok);  // on two headset ports, a talker still plays once
    std::vector<int16_t> out(4096);
    // Not started: silence, nothing consumed.
    CHECK(service.render_secondary(out.data(), 320) == 0 && out[0] == 0);
    service.start();
    size_t accepted = 0;
    auto a0 = one_block(0, 1), a1 = one_block(1, 1), b0 = one_block(0, 2), b1 = one_block(1, 2);
    service.write(a, a0.data(), a0.size(), accepted);
    service.write(b, b0.data(), b0.size(), accepted);
    clock.ms += 20;
    // One block each is under the 60 ms hold: silence.
    CHECK(service.render_secondary(out.data(), 320) == 0 && out[0] == 0 && out[319] == 0);
    service.write(a, a1.data(), a1.size(), accepted);
    service.write(b, b1.data(), b1.size(), accepted);
    clock.ms += 20;  // both talkers due 20 ms (320 samples)
    CHECK(service.render_secondary(out.data(), 640) == 320);
    // (100 + 200) / 32768 at the voice volume 0.5: 150; then silence after what was due.
    CHECK(out[0] == 150 && out[319] == 150 && out[320] == 0 && out[639] == 0);
    CHECK(service.receive_stats().samples_played == 320);
    // The port's volume and an input's mute apply; a muted port consumes and renders silence.
    service.set_volume(headset, 0.5f);
    service.set_mute(b, true);
    clock.ms += 10;
    CHECK(service.render_secondary(out.data(), 160) == 160 && out[0] == 25);  // 100 * 0.5 * 0.5
    service.set_mute(b, false);
    service.set_mute(headset, true);
    clock.ms += 10;
    CHECK(service.render_secondary(out.data(), 160) == 160 && out[0] == 0);
    service.set_mute(headset, false);
    service.set_volume(headset, 1.0f);
    // Real-time pull: everything due is played and nothing piles up or drops.
    for (int i = 0; i < 50; ++i) {
        auto x = one_block(uint8_t(2 + i), 1), y = one_block(uint8_t(2 + i), 2);
        service.write(a, x.data(), x.size(), accepted);
        service.write(b, y.data(), y.size(), accepted);
        clock.ms += 40;
        service.render_secondary(out.data(), 640);
    }
    const ReceiveStats steady = service.receive_stats();
    CHECK(steady.samples_dropped == 0);
    CHECK(steady.samples_played >= 320 + 320 + 49 * 640);
    // PCM the game writes (its jamming static) plays on the headset too, mixed down to mono.
    service.disconnect(a, headset);
    service.disconnect(a, other);
    service.disconnect(b, headset);
    CHECK(service.connect(static_in, headset) == status::Ok);
    std::vector<uint8_t> noise(2 * 320);
    for (size_t i = 0; i < 320; ++i) noise[2 * i] = 0x10, noise[2 * i + 1] = 0x00;  // 4096 big-endian
    CHECK(service.write(static_in, noise.data(), noise.size(), accepted) == status::Ok && accepted == noise.size());
    CHECK(service.render_secondary(out.data(), 400) == 320 && out[0] == 2048 && out[319] == 2048 && out[320] == 0);
    // At most playout_cap_ms of game PCM is kept.
    std::vector<uint8_t> lots(2 * 16000);
    service.write(static_in, lots.data(), lots.size(), accepted);
    CHECK(service.receive_stats().samples_dropped == 16000 - 300 * 16);
    service.end();
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

// The microphone port's and the encoded output's volumes reach the encoder: the tone's peak (8000)
// comes out at unity, halved at a microphone volume of 0.5, and scaled by the output's volume too.
class PeakEncoder final : public FrameEncoder {
public:
    explicit PeakEncoder(int* peak) : peak_(peak) {}
    size_t encode(uint64_t, const int16_t* samples, uint8_t* out, size_t capacity) override {
        for (unsigned i = 0; i < FrameSamples; ++i) *peak_ = std::max(*peak_, std::abs(int(samples[i])));
        std::memset(out, 1, capacity);
        return capacity;
    }

private:
    int* peak_;
};

static void port_volumes_reach_the_encoder() {
    const struct {
        float mic, out;
        int peak;
    } cases[] = {{1.0f, 1.0f, 8000}, {0.5f, 1.0f, 4000}, {1.0f, 0.25f, 2000}, {0.5f, 0.5f, 2000}, {2.0f, 3.0f, 32767}};
    for (const auto& c : cases) {
        Clock clock;
        int peak = 0;
        ServiceOptions options;
        options.now_ms = [&clock] { return clock.ms; };
        options.source = std::make_unique<TestToneSource>();
        options.encoder = std::make_unique<PeakEncoder>(&peak);
        Service service(std::move(options));
        service.init();
        PortParams mic_params;
        mic_params.type = uint32_t(PortType::InMic);
        uint32_t mic = 0, out = 0;
        service.create_port(mic_params, mic);
        service.create_port(voice_port(PortType::OutVoice), out);
        service.connect(mic, out);
        service.set_volume(mic, c.mic);
        service.set_volume(out, c.out);
        service.start();
        clock.ms += 400;
        PortInfo info;
        service.port_info(out, info);
        if (std::abs(peak - c.peak) > 2) std::printf("mic %.2f out %.2f: peak %d, want %d\n", c.mic, c.out, peak, c.peak);
        CHECK(std::abs(peak - c.peak) <= 2);
    }
}

// A microphone that is always open: push to talk held, or stuck.
class OpenSource final : public FrameSource {
public:
    bool frame(uint64_t, int16_t* samples) override {
        for (unsigned i = 0; i < FrameSamples; ++i) samples[i] = 4000;
        return true;
    }
};

// Signal state reports activity only for frames that put a block on an output the game reads, and
// an output the game stopped reading is discarded instead of reporting voice pending forever.
static void signal_state_follows_transmission() {
    Clock clock;
    std::vector<std::string> lines;
    ServiceOptions options;
    options.now_ms = [&clock] { return clock.ms; };
    options.source = std::make_unique<OpenSource>();
    options.encoder = std::make_unique<PatternEncoder>();
    options.log = [&lines](const char* line) { lines.emplace_back(line); };
    Service service(std::move(options));
    service.init();
    PortParams mic_params;
    mic_params.type = uint32_t(PortType::InMic);
    uint32_t mic = 0, out = 0;
    service.create_port(mic_params, mic);
    service.create_port(voice_port(PortType::OutVoice), out);
    float energy = 0;
    uint32_t vad = 0;
    bool is_float = false;
    const auto vad_now = [&] {
        vad = 7;
        CHECK(service.signal_state(mic, AttrVad, energy, vad, is_float) == status::Ok);
        return vad;
    };
    const auto energy_now = [&] {
        energy = -1;
        CHECK(service.signal_state(mic, AttrEnergyLevel, energy, vad, is_float) == status::Ok);
        return energy;
    };
    service.start();
    // The gate is open but the microphone feeds no output: nothing is sent, so no activity.
    clock.ms += 200;
    CHECK(vad_now() == 0 && energy_now() == 0);
    service.connect(mic, out);
    clock.ms += 200;
    CHECK(vad_now() == 1 && energy_now() > 0.1f);
    // A muted output sends nothing: no activity.
    service.set_mute(out, true);
    clock.ms += 80;
    CHECK(vad_now() == 0 && energy_now() == 0);
    service.set_mute(out, false);
    clock.ms += 80;
    CHECK(vad_now() == 1);
    // The game keeps reading: activity stays on as long as blocks go out.
    uint8_t buffer[1024];
    size_t copied = 0;
    for (int i = 0; i < 10; ++i) {
        clock.ms += 400;
        CHECK(service.read(out, buffer, sizeof buffer, copied) == status::Ok && copied > 0);
        CHECK(vad_now() == 1);
    }
    CHECK(lines.empty());
    // The game stops reading the output: after a second nothing is on the air, said once.
    clock.ms += 600;
    CHECK(vad_now() == 1);
    clock.ms += 600;
    CHECK(vad_now() == 0 && energy_now() == 0);
    clock.ms += 600;
    CHECK(vad_now() == 0);
    CHECK(lines.size() == 1 && lines[0].find("has not read it") != std::string::npos);
    // Reading again brings it back.
    CHECK(service.read(out, buffer, sizeof buffer, copied) == status::Ok && copied > 0);
    clock.ms += 40;
    CHECK(vad_now() == 1);
    CHECK(lines.size() == 2 && lines[1].find("reads the voice output again") != std::string::npos);
    service.stop();
    CHECK(vad_now() == 0);
    service.end();
}

// Encoded output the game stops reading after the gate closes: discarded once its newest block is
// older than stale_output_ms, so the port no longer reports voice waiting to be sent.
static void unread_output_expires() {
    Clock clock;
    std::vector<std::string> lines;
    ServiceOptions options;
    options.now_ms = [&clock] { return clock.ms; };
    options.source = std::make_unique<TestToneSource>();  // one second on, one off
    options.encoder = std::make_unique<PatternEncoder>();
    options.log = [&lines](const char* line) { lines.emplace_back(line); };
    options.stale_output_ms = 500;  // shorter than the source's silent second
    Service service(std::move(options));
    service.init();
    PortParams mic_params;
    mic_params.type = uint32_t(PortType::InMic);
    uint32_t mic = 0, out = 0;
    service.create_port(mic_params, mic);
    service.create_port(voice_port(PortType::OutVoice), out);
    service.connect(mic, out);
    service.start();
    PortInfo info;
    const auto step = [&](unsigned ms) {
        for (unsigned i = 0; i < ms / 40; ++i) {
            clock.ms += 40;
            service.port_info(out, info);
        }
    };
    step(1000);  // frames 0..24, open: the queue keeps its newest 480 ms
    CHECK(info.bytes == 12 * 37);
    step(480);   // closed since frame 25; the newest block is 480 ms old
    CHECK(info.bytes == 12 * 37);
    step(40);    // past 500 ms
    CHECK(info.bytes == 0);
    CHECK(service.send_stats().blocks_expired == 12);
    // One line for the discard (each open second is shorter than the read stall, so no stall line).
    CHECK(lines.size() == 1 && lines[0].find("did not read") != std::string::npos);
    step(1000);  // the next open second, still unread, and its discard: no new line
    step(1000);
    CHECK(info.bytes == 0 && service.send_stats().blocks_expired == 24);
    CHECK(lines.size() == 1);  // once per start
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
    playout_to_pcm_port();
    playout_to_secondary();
    trace_has_no_payload();
    port_volumes_reach_the_encoder();
    signal_state_follows_transmission();
    unread_output_expires();
    if (failures) {
        std::printf("%d failures\n", failures);
        return 1;
    }
    std::printf("PASS voice core\n");
    return 0;
}
