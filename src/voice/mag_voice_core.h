#pragma once
/* MAG voice service core (docs/superpowers/specs/2026-11-25-voice-comms-design.md).
 *
 * A host-side model of the voice library's port graph: ports, edges, encoded-voice output and
 * input queues framed as fixed-size voice blocks, and a signal source gated like push to talk.
 * It knows nothing about guest memory; mag_voice_hle.cpp translates the game's calls into it.
 * All values here are host-endian. */
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <functional>
#include <memory>
#include <mutex>
#include <vector>

namespace mag::voice {

// Status codes of the public voice library interface.
namespace status {
constexpr uint32_t Ok = 0;
constexpr uint32_t NotInitialized = 0x80310801u;
constexpr uint32_t AlreadyInitialized = 0x80310802u;
constexpr uint32_t ArgumentInvalid = 0x80310805u;
constexpr uint32_t Topology = 0x80310807u;
constexpr uint32_t ResourceInsufficient = 0x80310808u;
constexpr uint32_t DeviceNotPresent = 0x80310812u;
}  // namespace status

enum class PortType : uint32_t { InMic = 0, InPcm = 1, InVoice = 2, OutPcm = 3, OutVoice = 4, OutSecondary = 5 };
enum class PortState : uint32_t { Idle = 0, Ready = 1, Buffering = 2, Running = 3 };
// Signal-state attributes of the public interface.
constexpr uint32_t AttrEnergyLevel = 1000, AttrVad = 1001;

struct PortParams {
    uint32_t type = 0;
    uint16_t threshold = 0;
    uint16_t mute = 0;
    float volume = 1.0f;
    uint32_t bitrate = 0;       // voice ports
    uint32_t pcm_buffer = 0;    // PCM ports
    uint8_t pcm_channels = 0;
    uint8_t pcm_alignment = 0;
    uint32_t pcm_type = 0;
    uint32_t pcm_rate = 0;
};

struct PortInfo {
    uint32_t type = 0;
    uint32_t state = 0;
    std::vector<uint32_t> edges;
    uint32_t queued_bytes = 0;
    uint32_t frame_bytes = 0;
};

// --- Voice blocks -------------------------------------------------------------------------------
// One block per frame: marker, sequence, payload length, check byte (CRC-8 of the sequence, length
// and payload), payload, zero padding to a fixed size.
constexpr unsigned FrameMs = 40;
constexpr unsigned SampleRate = 16000;
constexpr unsigned FrameSamples = SampleRate * FrameMs / 1000;  // 640
constexpr uint8_t BlockMarker = 0xA1;                           // high nibble 0xA, format version 1
constexpr size_t BlockHeader = 4;
uint8_t block_check(uint8_t sequence, const uint8_t* payload, size_t payload_bytes);
// The block size that carries `bitrate` bits per second at one block per frame (at least 8 bytes).
size_t block_size_for_bitrate(uint32_t bitrate);
// Writes one block of `block` bytes to `out`. Returns 0 when the payload does not fit.
size_t write_block(uint8_t sequence, const uint8_t* payload, size_t payload_bytes, size_t block, uint8_t* out);

struct Block {
    uint8_t sequence = 0;
    std::vector<uint8_t> payload;
};

// Recovers blocks of one fixed size from a byte stream split or joined in any way. Bytes that do
// not start a valid block (marker, length within the block, check byte, zero padding) are skipped
// one at a time.
class BlockParser {
public:
    explicit BlockParser(size_t block) : block_(block) {}
    void push(const uint8_t* data, size_t bytes);
    bool next(Block& out);
    void reset() { pending_.clear(); }
    size_t pending() const { return pending_.size(); }
    uint64_t skipped_bytes() const { return skipped_; }
    size_t block_size() const { return block_; }

private:
    size_t block_;
    std::vector<uint8_t> pending_;
    uint64_t skipped_ = 0;
};

// --- Signal and codec ---------------------------------------------------------------------------
// A frame source: fills FrameSamples samples for frame `index` and returns whether the talk gate
// is open for it.
class FrameSource {
public:
    virtual ~FrameSource() = default;
    virtual bool frame(uint64_t index, int16_t* samples) = 0;
};

// The lab signal: one second of a 440 Hz tone, then one second of silence, the gate following
// the tone.
class TestToneSource final : public FrameSource {
public:
    bool frame(uint64_t index, int16_t* samples) override;
};

class FrameEncoder {
public:
    virtual ~FrameEncoder() = default;
    // Encodes one frame into at most `capacity` bytes; returns the bytes written (0 drops it).
    virtual size_t encode(uint64_t index, const int16_t* samples, uint8_t* out, size_t capacity) = 0;
};

// The lab payload: the frame index (4 bytes, big-endian) then bytes counting up from it, filling
// the block, so a receiver can check that the path preserved every byte and the order.
class PatternEncoder final : public FrameEncoder {
public:
    size_t encode(uint64_t index, const int16_t* samples, uint8_t* out, size_t capacity) override;
};
// True when `payload` is exactly what PatternEncoder writes for its own frame index.
bool pattern_payload_valid(const std::vector<uint8_t>& payload);

// The parameters as JSON fields, for trace lines.
void describe_params(char* text, size_t size, const PortParams& params);

// --- Trace --------------------------------------------------------------------------------------
// JSON lines of port calls: sizes, counts and times, never sample or payload data.
class Trace {
public:
    explicit Trace(std::FILE* stream) : stream_(stream) {}
    ~Trace();
    static std::unique_ptr<Trace> open(const char* path);
    // `fields` is the inside of a JSON object without braces, e.g. "\"port\":3".
    void event(uint64_t ms, const char* name, const char* fields);

private:
    std::FILE* stream_;
    std::mutex lock_;
};

// --- Service ------------------------------------------------------------------------------------
struct ReceiveStats {
    uint64_t write_calls = 0, bytes_written = 0, blocks = 0, pattern_ok = 0, pattern_bad = 0, sequence_gaps = 0,
             skipped_bytes = 0;
};
struct SendStats {
    uint64_t frames = 0, blocks = 0, blocks_dropped = 0, read_calls = 0, bytes_read = 0;
};

struct ServiceOptions {
    std::function<uint64_t()> now_ms;  // monotonic milliseconds
    std::unique_ptr<FrameSource> source;
    std::unique_ptr<FrameEncoder> encoder;
    std::unique_ptr<Trace> trace;
    unsigned queue_ms = 480;       // encoded output kept before the oldest blocks are dropped
    unsigned max_catch_up = 12;    // frames produced at most per pump after a stall
    size_t input_capacity = 4096;  // bytes an encoded input port holds unparsed
};

class Service {
public:
    explicit Service(ServiceOptions options);

    uint32_t init();
    uint32_t end();
    uint32_t start();
    uint32_t stop();
    uint32_t create_port(const PortParams& params, uint32_t& id);
    uint32_t delete_port(uint32_t id);
    uint32_t connect(uint32_t input, uint32_t output);
    uint32_t disconnect(uint32_t input, uint32_t output);
    uint32_t reset_port(uint32_t id);
    uint32_t update_port(uint32_t id, const PortParams& params);
    uint32_t set_mute(uint32_t id, bool mute);
    uint32_t mute(uint32_t id, bool& mute);
    uint32_t set_volume(uint32_t id, float volume);
    uint32_t volume(uint32_t id, float& volume);
    uint32_t port_info(uint32_t id, PortInfo& info);
    // Copies whole blocks, at most `capacity` bytes; `copied` receives the count.
    uint32_t read(uint32_t id, uint8_t* out, size_t capacity, size_t& copied);
    // Accepts up to the port's free space; `accepted` receives the count.
    uint32_t write(uint32_t id, const uint8_t* data, size_t bytes, size_t& accepted);
    // Writes the attribute's value: energy as a level in [0, 1], voice activity as 0 or 1.
    uint32_t signal_state(uint32_t id, uint32_t attr, float& energy, uint32_t& vad, bool& is_float);

    SendStats send_stats();
    ReceiveStats receive_stats();
    void trace_event(const char* name, const char* fields);
    bool tracing() const { return options_.trace != nullptr; }

private:
    struct Port {
        bool used = false;
        uint32_t id = 0;
        PortParams params;
        std::vector<uint32_t> edges;  // ports this one connects to (input side) or from (output side)
        std::vector<uint8_t> queue;   // encoded output bytes
        std::unique_ptr<BlockParser> parser;
        bool have_sequence = false;
        uint8_t last_sequence = 0;
    };
    Port* find(uint32_t id);
    void pump();          // produce the frames due since the last call
    void summary(uint64_t now, bool force = false);
    uint64_t now() const { return options_.now_ms ? options_.now_ms() : 0; }
    uint64_t trace_ms() const { return now() - epoch_; }

    ServiceOptions options_;
    std::mutex lock_;
    bool initialized_ = false, started_ = false, gate_ = false;
    uint64_t epoch_ = 0, started_at_ = 0, frames_done_ = 0, next_summary_ = 0;
    float energy_ = 0;
    uint32_t serial_ = 0;
    std::vector<Port> ports_;
    SendStats send_;
    ReceiveStats receive_;
};

}  // namespace mag::voice
