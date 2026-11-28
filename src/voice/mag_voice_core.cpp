#include "mag_voice_core.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>

namespace mag::voice {

// The interface allows 128 ports; the game creates a pair per remote talker slot plus its own.
constexpr size_t MaxPorts = 128;

size_t block_size_for_bitrate(uint32_t bitrate) {
    const size_t bytes = (static_cast<size_t>(bitrate) * FrameMs + 7999) / 8000;
    return std::max<size_t>(bytes, 8);
}

uint8_t block_check(uint8_t sequence, const uint8_t* payload, size_t payload_bytes) {
    // CRC-8, polynomial x^8 + x^2 + x + 1, over the sequence, the length and the payload.
    uint8_t crc = 0;
    auto feed = [&crc](uint8_t byte) {
        crc ^= byte;
        for (int bit = 0; bit < 8; ++bit) crc = static_cast<uint8_t>((crc & 0x80) ? (crc << 1) ^ 0x07 : crc << 1);
    };
    feed(sequence);
    feed(static_cast<uint8_t>(payload_bytes));
    for (size_t i = 0; i < payload_bytes; ++i) feed(payload[i]);
    return crc;
}

size_t write_block(uint8_t sequence, const uint8_t* payload, size_t payload_bytes, size_t block, uint8_t* out) {
    if (block < BlockHeader || payload_bytes > block - BlockHeader || payload_bytes > 255) return 0;
    out[0] = BlockMarker;
    out[1] = sequence;
    out[2] = static_cast<uint8_t>(payload_bytes);
    out[3] = block_check(sequence, payload, payload_bytes);
    if (payload_bytes) std::memcpy(out + BlockHeader, payload, payload_bytes);
    std::memset(out + BlockHeader + payload_bytes, 0, block - BlockHeader - payload_bytes);
    return block;
}

void BlockParser::push(const uint8_t* data, size_t bytes) { pending_.insert(pending_.end(), data, data + bytes); }

bool BlockParser::next(Block& out) {
    size_t start = 0;
    bool found = false;
    while (pending_.size() - start >= block_) {
        const uint8_t* candidate = pending_.data() + start;
        const size_t length = candidate[2];
        const bool header = candidate[0] == BlockMarker && length <= block_ - BlockHeader;
        const bool checked = header && candidate[3] == block_check(candidate[1], candidate + BlockHeader, length);
        const bool padding = checked && std::all_of(candidate + BlockHeader + length, candidate + block_,
                                                   [](uint8_t b) { return b == 0; });
        if (padding) {
            out.sequence = candidate[1];
            out.payload.assign(candidate + BlockHeader, candidate + BlockHeader + length);
            start += block_;
            found = true;
            break;
        }
        ++start;
        ++skipped_;
    }
    // A marker may still begin in the unfinished tail; keep it for the next push.
    pending_.erase(pending_.begin(), pending_.begin() + static_cast<std::ptrdiff_t>(start));
    return found;
}

bool TestToneSource::frame(uint64_t index, int16_t* samples) {
    const uint64_t frames_per_second = 1000 / FrameMs;
    const bool on = (index / frames_per_second) % 2 == 0;
    for (unsigned i = 0; i < FrameSamples; ++i) {
        if (!on) {
            samples[i] = 0;
            continue;
        }
        const double t = static_cast<double>(index * FrameSamples + i) / SampleRate;
        samples[i] = static_cast<int16_t>(std::lround(8000.0 * std::sin(2.0 * 3.14159265358979323846 * 440.0 * t)));
    }
    return on;
}

size_t PatternEncoder::encode(uint64_t index, const int16_t*, uint8_t* out, size_t capacity) {
    if (capacity < 4) return 0;
    const uint32_t value = static_cast<uint32_t>(index);
    out[0] = static_cast<uint8_t>(value >> 24);
    out[1] = static_cast<uint8_t>(value >> 16);
    out[2] = static_cast<uint8_t>(value >> 8);
    out[3] = static_cast<uint8_t>(value);
    for (size_t i = 4; i < capacity; ++i) out[i] = static_cast<uint8_t>(value + i);
    return capacity;
}

bool pattern_payload_valid(const std::vector<uint8_t>& payload) {
    if (payload.size() < 4) return false;
    const uint32_t value = (uint32_t(payload[0]) << 24) | (uint32_t(payload[1]) << 16) | (uint32_t(payload[2]) << 8) |
                           uint32_t(payload[3]);
    for (size_t i = 4; i < payload.size(); ++i)
        if (payload[i] != static_cast<uint8_t>(value + i)) return false;
    return true;
}

Trace::~Trace() {
    if (stream_) std::fclose(stream_);
}

std::unique_ptr<Trace> Trace::open(const char* path) {
    if (!path || !*path) return nullptr;
    std::FILE* stream = std::fopen(path, "w");
    if (!stream) {
        std::fprintf(stderr, "[voice] cannot write the trace %s\n", path);
        return nullptr;
    }
    return std::make_unique<Trace>(stream);
}

void Trace::event(uint64_t ms, const char* name, const char* fields) {
    std::lock_guard<std::mutex> hold(lock_);
    std::fprintf(stream_, "{\"ms\":%llu,\"event\":\"%s\"%s%s}\n", static_cast<unsigned long long>(ms), name,
                 fields && *fields ? "," : "", fields ? fields : "");
    std::fflush(stream_);
}

Service::Service(ServiceOptions options) : options_(std::move(options)) {}

Service::Port* Service::find(uint32_t id) {
    for (auto& port : ports_)
        if (port.used && port.id == id) return &port;
    return nullptr;
}

static bool voice_type(uint32_t type) {
    return type == uint32_t(PortType::InVoice) || type == uint32_t(PortType::OutVoice);
}

void describe_params(char* text, size_t size, const PortParams& p) {
    std::snprintf(text, size,
                  "\"type\":%u,\"threshold\":%u,\"mute\":%u,\"volume\":%.4f,\"bitrate\":%u,\"pcm_buffer\":%u,"
                  "\"pcm_channels\":%u,\"pcm_alignment\":%u,\"pcm_type\":%u,\"pcm_rate\":%u",
                  p.type, p.threshold, p.mute, static_cast<double>(p.volume), p.bitrate, p.pcm_buffer, p.pcm_channels,
                  p.pcm_alignment, p.pcm_type, p.pcm_rate);
}

uint32_t Service::init() {
    std::lock_guard<std::mutex> hold(lock_);
    if (initialized_) return status::AlreadyInitialized;
    initialized_ = true;
    started_ = false;
    ports_.clear();
    ports_.resize(MaxPorts);
    epoch_ = now();
    next_summary_ = epoch_ + 5000;
    return status::Ok;
}

uint32_t Service::end() {
    std::lock_guard<std::mutex> hold(lock_);
    if (!initialized_) return status::NotInitialized;
    summary(now(), true);
    if (started_ && options_.source) options_.source->stop();
    initialized_ = false;
    started_ = false;
    gate_ = sent_ = false;
    energy_ = 0;
    ports_.clear();
    return status::Ok;
}

uint32_t Service::start() {
    std::lock_guard<std::mutex> hold(lock_);
    if (!initialized_) return status::NotInitialized;
    if (!started_) {
        started_ = true;
        started_at_ = last_output_read_ = now();
        frames_done_ = 0;
        sent_ = stall_reported_ = expiry_reported_ = false;
        if (options_.source) options_.source->start();
    }
    return status::Ok;
}

uint32_t Service::stop() {
    std::lock_guard<std::mutex> hold(lock_);
    if (!initialized_) return status::NotInitialized;
    if (started_ && options_.source) options_.source->stop();
    started_ = false;
    gate_ = sent_ = false;
    energy_ = 0;
    return status::Ok;
}

uint32_t Service::create_port(const PortParams& params, uint32_t& id) {
    std::lock_guard<std::mutex> hold(lock_);
    if (!initialized_) return status::NotInitialized;
    if (params.type > uint32_t(PortType::OutSecondary)) return status::ArgumentInvalid;
    for (size_t slot = 0; slot < ports_.size(); ++slot) {
        Port& port = ports_[slot];
        if (port.used) continue;
        port = Port{};
        port.used = true;
        // Slot in the high byte, a wrapping creation count in the low byte: never 0, never reused at once.
        port.id = (uint32_t(slot + 1) << 8) | (serial_++ % 0xA0u);
        port.params = params;
        if (params.type == uint32_t(PortType::InVoice)) {
            port.parser = std::make_unique<BlockParser>(block_size_for_bitrate(params.bitrate));
            if (options_.decoder) port.decoder = options_.decoder();
        }
        id = port.id;
        return status::Ok;
    }
    return status::ResourceInsufficient;
}

uint32_t Service::delete_port(uint32_t id) {
    std::lock_guard<std::mutex> hold(lock_);
    if (!initialized_) return status::NotInitialized;
    Port* port = find(id);
    if (!port) return status::Topology;
    for (auto& other : ports_) other.edges.erase(std::remove(other.edges.begin(), other.edges.end(), id), other.edges.end());
    *port = Port{};
    return status::Ok;
}

uint32_t Service::connect(uint32_t input, uint32_t output) {
    std::lock_guard<std::mutex> hold(lock_);
    if (!initialized_) return status::NotInitialized;
    Port* in = find(input);
    Port* out = find(output);
    if (!in || !out || in->params.type >= uint32_t(PortType::OutPcm) || out->params.type <= uint32_t(PortType::InVoice))
        return status::Topology;
    if (std::find(in->edges.begin(), in->edges.end(), output) == in->edges.end()) in->edges.push_back(output);
    if (std::find(out->edges.begin(), out->edges.end(), input) == out->edges.end()) out->edges.push_back(input);
    return status::Ok;
}

uint32_t Service::disconnect(uint32_t input, uint32_t output) {
    std::lock_guard<std::mutex> hold(lock_);
    if (!initialized_) return status::NotInitialized;
    Port* in = find(input);
    Port* out = find(output);
    if (!in || !out || in->params.type >= uint32_t(PortType::OutPcm) || out->params.type <= uint32_t(PortType::InVoice))
        return status::Topology;
    in->edges.erase(std::remove(in->edges.begin(), in->edges.end(), output), in->edges.end());
    out->edges.erase(std::remove(out->edges.begin(), out->edges.end(), input), out->edges.end());
    return status::Ok;
}

uint32_t Service::reset_port(uint32_t id) {
    std::lock_guard<std::mutex> hold(lock_);
    if (!initialized_) return status::NotInitialized;
    Port* port = find(id);
    if (!port) return status::Topology;
    port->queue.clear();
    if (port->parser) port->parser->reset();
    port->have_sequence = false;
    port->have_voice = false;  // a new talker: not talking until its first block
    // A new talker on the port: forget the old one's audio and decoder state.
    port->pending.clear();
    port->ready.clear();
    port->playing = false;
    if (port->decoder && options_.decoder) port->decoder = options_.decoder();
    return status::Ok;
}

uint32_t Service::update_port(uint32_t id, const PortParams& params) {
    std::lock_guard<std::mutex> hold(lock_);
    if (!initialized_) return status::NotInitialized;
    Port* port = find(id);
    if (!port) return status::Topology;
    // The type and PCM format stay as created; threshold, mute, volume and a voice port's rate change.
    port->params.threshold = params.threshold;
    port->params.mute = params.mute;
    port->params.volume = params.volume;
    if (voice_type(port->params.type) && params.bitrate != port->params.bitrate) {
        port->params.bitrate = params.bitrate;
        port->queue.clear();
        if (port->parser) port->parser = std::make_unique<BlockParser>(block_size_for_bitrate(params.bitrate));
    }
    return status::Ok;
}

uint32_t Service::set_mute(uint32_t id, bool mute) {
    std::lock_guard<std::mutex> hold(lock_);
    if (!initialized_) return status::NotInitialized;
    Port* port = find(id);
    if (!port) return status::Topology;
    port->params.mute = mute ? 1 : 0;
    return status::Ok;
}

uint32_t Service::mute(uint32_t id, bool& mute) {
    std::lock_guard<std::mutex> hold(lock_);
    if (!initialized_) return status::NotInitialized;
    Port* port = find(id);
    if (!port) return status::Topology;
    mute = port->params.mute != 0;
    return status::Ok;
}

uint32_t Service::set_volume(uint32_t id, float volume) {
    std::lock_guard<std::mutex> hold(lock_);
    if (!initialized_) return status::NotInitialized;
    Port* port = find(id);
    if (!port) return status::Topology;
    port->params.volume = volume;
    return status::Ok;
}

uint32_t Service::volume(uint32_t id, float& volume) {
    std::lock_guard<std::mutex> hold(lock_);
    if (!initialized_) return status::NotInitialized;
    Port* port = find(id);
    if (!port) return status::Topology;
    volume = port->params.volume;
    return status::Ok;
}

// Bytes per sample frame (all channels) of a PCM port: float and 32-bit integer 4 bytes a channel,
// 16-bit integer 2.
static uint32_t pcm_sample_bytes(const PortParams& p) {
    const uint32_t sample = (p.pcm_type == 2 || p.pcm_type == 3) ? 2 : 4;
    return sample * std::max<uint32_t>(p.pcm_channels, 1);
}
static uint32_t pcm_frame_bytes(const PortParams& p) { return FrameSamples * pcm_sample_bytes(p); }

// Writes one sample in a PCM port's format: types 0/1 float, 2/3 16-bit, 4/5 32-bit; odd types
// little-endian, even types big-endian (the console's order).
static void put_sample(uint8_t* out, const PortParams& p, float value) {
    value = std::max(-1.0f, std::min(1.0f, value));
    uint32_t bits = 0;
    unsigned width = 4;
    if (p.pcm_type == 2 || p.pcm_type == 3) {
        bits = static_cast<uint16_t>(static_cast<int16_t>(std::lround(value * 32767.0f)));
        width = 2;
    } else if (p.pcm_type == 4 || p.pcm_type == 5) {
        bits = static_cast<uint32_t>(static_cast<int32_t>(std::llround(double(value) * 2147483647.0)));
    } else {
        std::memcpy(&bits, &value, 4);
    }
    const bool little = (p.pcm_type & 1u) != 0;
    for (unsigned i = 0; i < width; ++i) out[i] = uint8_t(bits >> (8 * (little ? i : width - 1 - i)));
}

// Reads one sample in a PCM port's format (the inverse of put_sample), as a value in [-1, 1].
static float get_sample(const uint8_t* in, const PortParams& p) {
    const bool little = (p.pcm_type & 1u) != 0;
    const unsigned width = (p.pcm_type == 2 || p.pcm_type == 3) ? 2 : 4;
    uint32_t bits = 0;
    for (unsigned i = 0; i < width; ++i) bits |= uint32_t(in[i]) << (8 * (little ? i : width - 1 - i));
    if (width == 2) return float(static_cast<int16_t>(bits)) / 32768.0f;
    if (p.pcm_type == 4 || p.pcm_type == 5) return float(double(static_cast<int32_t>(bits)) / 2147483648.0);
    float value;
    std::memcpy(&value, &bits, 4);
    return value;
}

uint32_t Service::port_info(uint32_t id, PortInfo& info) {
    std::lock_guard<std::mutex> hold(lock_);
    if (!initialized_) return status::NotInitialized;
    pump();
    Port* port = find(id);
    if (!port) return status::Topology;
    info.type = port->params.type;
    info.state = port_state(*port, now());
    info.edges = port->edges;
    if (voice_type(port->params.type)) {
        info.frame_bytes = static_cast<uint32_t>(block_size_for_bitrate(port->params.bitrate));
        if (port->parser) {
            const size_t pending = port->parser->pending();
            info.bytes = static_cast<uint32_t>(options_.input_capacity > pending ? options_.input_capacity - pending : 0);
        } else {
            info.bytes = static_cast<uint32_t>(port->queue.size());
        }
    } else if (port->params.type == uint32_t(PortType::OutPcm)) {
        info.frame_bytes = pcm_frame_bytes(port->params);
        info.bytes = static_cast<uint32_t>(pcm_ready(*port) * pcm_sample_bytes(port->params));
    } else if (port->params.type == uint32_t(PortType::InPcm)) {
        info.frame_bytes = pcm_frame_bytes(port->params);
        info.bytes = std::max<uint32_t>(port->params.pcm_buffer, info.frame_bytes);
    } else {
        info.frame_bytes = 0;
        info.bytes = 0;
    }
    return status::Ok;
}

uint32_t Service::read(uint32_t id, uint8_t* out, size_t capacity, size_t& copied) {
    std::lock_guard<std::mutex> hold(lock_);
    copied = 0;
    if (!initialized_) return status::NotInitialized;
    pump();
    Port* port = find(id);
    if (!port || port->params.type <= uint32_t(PortType::InVoice)) return status::Topology;
    ++send_.read_calls;
    if (port->params.type == uint32_t(PortType::OutPcm)) {
        const uint32_t frame = pcm_sample_bytes(port->params);
        const size_t count = std::min(pcm_ready(*port), capacity / frame);
        if (!count || !out) return status::Ok;
        const unsigned channels = std::max<unsigned>(port->params.pcm_channels, 1);
        const float out_gain = port->params.mute ? 0.0f : port->params.volume * options_.voice_gain;
        const size_t width = frame / channels;
        for (size_t i = 0; i < count; ++i) {
            float mix = 0;
            for (uint32_t input : port->edges) {
                Port* in = find(input);
                if (!in || i >= in->ready.size() || in->params.mute) continue;
                mix += float(in->ready[i]) / 32768.0f * in->params.volume;
            }
            for (unsigned c = 0; c < channels; ++c) put_sample(out + i * frame + c * width, port->params, mix * out_gain);
        }
        for (uint32_t input : port->edges) {
            Port* in = find(input);
            if (!in) continue;
            const size_t used = std::min(count, in->ready.size());
            in->ready.erase(in->ready.begin(), in->ready.begin() + static_cast<std::ptrdiff_t>(used));
        }
        copied = count * frame;
        receive_.samples_played += count;
        return status::Ok;
    }
    if (port->params.type == uint32_t(PortType::OutVoice)) {
        last_output_read_ = now();
        if (stall_reported_) {
            stall_reported_ = false;
            note("the game reads the voice output again");
        }
        const size_t block = block_size_for_bitrate(port->params.bitrate);
        const size_t whole = std::min(capacity, port->queue.size()) / block * block;
        if (whole && out) {
            std::memcpy(out, port->queue.data(), whole);
            port->queue.erase(port->queue.begin(), port->queue.begin() + static_cast<std::ptrdiff_t>(whole));
            copied = whole;
            send_.bytes_read += whole;
        }
    }
    return status::Ok;
}

uint32_t Service::write(uint32_t id, const uint8_t* data, size_t bytes, size_t& accepted) {
    std::lock_guard<std::mutex> hold(lock_);
    accepted = 0;
    if (!initialized_) return status::NotInitialized;
    pump();
    Port* port = find(id);
    if (!port || port->params.type >= uint32_t(PortType::OutPcm)) return status::Topology;
    ++receive_.write_calls;
    if (!port->parser) {
        // PCM from the game (the static it plays under jamming): whole samples join the port's audio,
        // which plays on whatever output it is connected to; the newest playout_cap_ms are kept.
        accepted = bytes;
        if (!data || port->params.type != uint32_t(PortType::InPcm)) return status::Ok;
        const uint32_t frame = pcm_sample_bytes(port->params);
        const unsigned channels = std::max<unsigned>(port->params.pcm_channels, 1);
        const size_t width = frame / channels;
        for (size_t at = 0; at + frame <= bytes; at += frame) {
            float sum = 0;
            for (unsigned c = 0; c < channels; ++c) sum += get_sample(data + at + c * width, port->params);
            const float value = std::max(-1.0f, std::min(1.0f, sum / float(channels)));
            port->ready.push_back(static_cast<int16_t>(std::lround(value * 32767.0f)));
        }
        const size_t cap = size_t(options_.playout_cap_ms) * SampleRate / 1000;
        if (port->ready.size() > cap) {
            const size_t drop = port->ready.size() - cap;
            port->ready.erase(port->ready.begin(), port->ready.begin() + static_cast<std::ptrdiff_t>(drop));
            receive_.samples_dropped += drop;
        }
        return status::Ok;
    }
    const size_t room = options_.input_capacity > port->parser->pending() ? options_.input_capacity - port->parser->pending() : 0;
    accepted = std::min(bytes, room);
    if (accepted && data) port->parser->push(data, accepted);
    receive_.bytes_written += accepted;
    const uint64_t skipped_before = port->parser->skipped_bytes();
    Block block;
    while (port->parser->next(block)) {
        ++receive_.blocks;
        if (pattern_payload_valid(block.payload)) ++receive_.pattern_ok; else ++receive_.pattern_bad;
        decode_block(*port, block);
        port->have_sequence = true;
        port->last_sequence = block.sequence;
        port->have_voice = true;
        port->last_voice = now();
    }
    port->last_write = now();
    if (started_) advance_playout(port->last_write);  // a talker that reaches its hold starts now
    receive_.skipped_bytes += port->parser->skipped_bytes() - skipped_before;
    return status::Ok;
}

uint32_t Service::signal_state(uint32_t id, uint32_t attr, float& energy, uint32_t& vad, bool& is_float) {
    std::lock_guard<std::mutex> hold(lock_);
    if (!initialized_) return status::NotInitialized;
    pump();
    if (!find(id)) return status::Topology;
    const bool live = transmitting(now());
    if (attr == AttrEnergyLevel) {
        energy = live ? energy_ : 0.0f;
        is_float = true;
        return status::Ok;
    }
    if (attr == AttrVad) {
        vad = live ? 1 : 0;
        is_float = false;
        return status::Ok;
    }
    if (attr == AttrSpeechLevel) {
        // The game keeps its microphone active (and sends what it reads) only while this level is at
        // or above its sensitivity threshold; push to talk has already decided, so report the top of
        // the scale while transmitting.
        vad = live ? SpeechLevelTop : 0;
        is_float = false;
        return status::Ok;
    }
    return status::DeviceNotPresent;  // as the runtime stub answers every other attribute
}

void Service::decode_block(Port& port, const Block& block) {
    const unsigned gap = port.have_sequence ? static_cast<uint8_t>(block.sequence - port.last_sequence - 1) : 0;
    if (gap) ++receive_.sequence_gaps;
    if (!port.decoder) return;
    std::vector<int16_t> samples(FrameSamples);
    if (gap > options_.max_concealed) {
        // Too much lost to bridge: start the talker again from this block.
        port.pending.clear();
        port.playing = false;
    } else {
        for (unsigned i = 0; i < gap; ++i)
            if (port.decoder->decode(nullptr, 0, samples.data())) {
                port.pending.insert(port.pending.end(), samples.begin(), samples.end());
                ++receive_.frames_concealed;
            }
    }
    if (!port.decoder->decode(block.payload.data(), block.payload.size(), samples.data())) {
        ++receive_.decode_errors;
        return;
    }
    ++receive_.frames_decoded;
    port.pending.insert(port.pending.end(), samples.begin(), samples.end());
    const size_t cap = size_t(options_.playout_cap_ms) * SampleRate / 1000;
    if (port.pending.size() > cap) {
        const size_t drop = port.pending.size() - cap;
        port.pending.erase(port.pending.begin(), port.pending.begin() + static_cast<std::ptrdiff_t>(drop));
        receive_.samples_dropped += drop;
    }
}

// Moves each talker's decoded audio to its PCM output as real time passes, after a short hold so
// network bursts play smoothly. A talker that runs dry stops and holds again when audio returns.
void Service::advance_playout(uint64_t t) {
    const size_t hold = size_t(options_.jitter_ms) * SampleRate / 1000;
    const size_t cap = size_t(options_.playout_cap_ms) * SampleRate / 1000;
    for (auto& port : ports_) {
        if (!port.used || !port.decoder) continue;
        if (!port.playing) {
            const bool held = port.pending.size() >= hold || (!port.pending.empty() && t - port.last_write >= options_.jitter_ms);
            if (!held) continue;
            port.playing = true;
            port.play_start = t;
            port.released = 0;
        }
        const uint64_t due = (t - port.play_start) * SampleRate / 1000;
        const size_t want = due > port.released ? static_cast<size_t>(due - port.released) : 0;
        const size_t moved = std::min(want, port.pending.size());
        port.ready.insert(port.ready.end(), port.pending.begin(), port.pending.begin() + static_cast<std::ptrdiff_t>(moved));
        port.pending.erase(port.pending.begin(), port.pending.begin() + static_cast<std::ptrdiff_t>(moved));
        port.released += moved;
        if (port.ready.size() > cap) {
            // Nobody reads this talker's output: keep only the newest audio.
            const size_t drop = port.ready.size() - cap;
            port.ready.erase(port.ready.begin(), port.ready.begin() + static_cast<std::ptrdiff_t>(drop));
            receive_.samples_dropped += drop;
        }
        if (port.pending.empty() && want > moved) {
            port.playing = false;
            ++receive_.underruns;
        }
    }
}

size_t Service::render_secondary(int16_t* out, size_t count) {
    std::lock_guard<std::mutex> hold(lock_);
    std::fill(out, out + count, int16_t(0));
    if (!initialized_ || !started_ || !count) return 0;
    advance_playout(now());
    mix_.assign(count, 0.0f);
    mixed_.clear();
    size_t carried = 0;
    for (auto& secondary : ports_) {
        if (!secondary.used || secondary.params.type != uint32_t(PortType::OutSecondary)) continue;
        const float out_gain = secondary.params.mute ? 0.0f : secondary.params.volume;
        for (uint32_t input : secondary.edges) {
            // An input on two secondary ports plays once.
            if (std::find(mixed_.begin(), mixed_.end(), input) != mixed_.end()) continue;
            mixed_.push_back(input);
            Port* in = find(input);
            if (!in) continue;
            const size_t n = std::min(count, in->ready.size());
            const float gain = in->params.mute ? 0.0f : in->params.volume * out_gain;
            if (gain != 0.0f)
                for (size_t i = 0; i < n; ++i) mix_[i] += float(in->ready[i]) / 32768.0f * gain;
            in->ready.erase(in->ready.begin(), in->ready.begin() + static_cast<std::ptrdiff_t>(n));
            carried = std::max(carried, n);
        }
    }
    for (size_t i = 0; i < count; ++i) {
        const float value = std::max(-1.0f, std::min(1.0f, mix_[i] * options_.voice_gain));
        out[i] = static_cast<int16_t>(std::lround(value * 32767.0f));
    }
    receive_.samples_played += carried;
    return carried;
}

// Talker samples due on a PCM output: the most any connected input has ready.
size_t Service::pcm_ready(const Port& output) {
    size_t most = 0;
    for (uint32_t input : output.edges)
        if (const Port* in = find(input)) most = std::max(most, in->ready.size());
    return most;
}

void Service::pump() {
    if (!started_) return;
    const uint64_t t = now();
    advance_playout(t);
    if (!options_.source) {
        summary(t);
        return;
    }
    const uint64_t due = (t - started_at_) / FrameMs;
    if (due <= frames_done_) {
        summary(t);
        return;
    }
    if (due - frames_done_ > options_.max_catch_up) frames_done_ = due - options_.max_catch_up;  // skip a stall
    std::vector<int16_t> samples(FrameSamples), scaled(FrameSamples);
    std::vector<uint8_t> payload, block;
    for (; frames_done_ < due; ++frames_done_) {
        const uint64_t index = frames_done_;
        gate_ = options_.source->frame(index, samples.data());
        double sum = 0;
        for (int16_t s : samples) sum += double(s) * double(s);
        energy_ = static_cast<float>(std::sqrt(sum / FrameSamples) / 32768.0);
        last_frame_at_ = t;
        const bool was_sent = sent_;
        sent_ = false;
        if (!gate_) continue;
        ++send_.frames;
        for (auto& out : ports_) {
            if (!out.used || out.params.type != uint32_t(PortType::OutVoice) || out.params.mute) continue;
            // The port carries its microphone inputs after each input's volume, at its own volume.
            bool fed = false;
            float gain = 0;
            for (uint32_t input : out.edges) {
                const Port* in = find(input);
                if (!in || in->params.type != uint32_t(PortType::InMic) || in->params.mute) continue;
                fed = true;
                gain += in->params.volume;
            }
            if (!fed) continue;
            gain *= out.params.volume;
            const int16_t* frame = samples.data();
            if (gain != 1.0f) {
                for (unsigned i = 0; i < FrameSamples; ++i)
                    scaled[i] = static_cast<int16_t>(std::clamp(std::lround(samples[i] * double(gain)), -32768L, 32767L));
                frame = scaled.data();
            }
            const size_t size = block_size_for_bitrate(out.params.bitrate);
            payload.assign(std::min<size_t>(size - BlockHeader, 255), 0);
            const size_t n = options_.encoder ? options_.encoder->encode(index, frame, payload.data(), payload.size()) : 0;
            if (!n) continue;
            block.resize(size);
            write_block(out.next_sequence++, payload.data(), n, size, block.data());
            out.queue.insert(out.queue.end(), block.begin(), block.end());
            out.last_block = t;
            if (!sent_ && !was_sent) sending_since_ = t;
            sent_ = true;
            ++send_.blocks;
            const size_t cap = std::max<size_t>(size, size * (options_.queue_ms / FrameMs));
            while (out.queue.size() > cap) {
                out.queue.erase(out.queue.begin(), out.queue.begin() + static_cast<std::ptrdiff_t>(size));
                ++send_.blocks_dropped;
            }
        }
    }
    expire_output(t);
    summary(t);
}

// Port state as the game reads it (docs/voice-chat.md, "Talk state"). The microphone runs only while
// it transmits and is otherwise ready, so the game's talk state ends shortly after push to talk
// closes. An encoded input runs while its talker is talking and is otherwise ready, so the game ends
// the talker once its voice has played out. Other ports run while the service runs.
uint32_t Service::port_state(const Port& port, uint64_t t) const {
    if (!started_) return uint32_t(PortState::Idle);
    if (port.params.type == uint32_t(PortType::InMic))
        return uint32_t(transmitting(t) ? PortState::Running : PortState::Ready);
    if (port.params.type == uint32_t(PortType::InVoice)) {
        const bool talking = port.playing || !port.pending.empty() ||
                             (port.have_voice && t - port.last_voice < options_.talker_linger_ms);
        return uint32_t(talking ? PortState::Running : PortState::Ready);
    }
    return uint32_t(PortState::Running);
}

bool Service::transmitting(uint64_t t) const {
    if (!started_ || !sent_ || t - last_frame_at_ > options_.signal_fresh_ms) return false;
    return t - std::max(last_output_read_, sending_since_) <= options_.read_stall_ms;
}

void Service::expire_output(uint64_t t) {
    if (sent_ && !stall_reported_ && t - std::max(last_output_read_, sending_since_) > options_.read_stall_ms) {
        stall_reported_ = true;
        char line[160];
        std::snprintf(line, sizeof line, "voice is being encoded but the game has not read it for %u ms; nothing is being sent",
                      options_.read_stall_ms);
        note(line);
    }
    for (auto& out : ports_) {
        if (!out.used || out.params.type != uint32_t(PortType::OutVoice) || out.queue.empty()) continue;
        if (t - out.last_block <= options_.stale_output_ms) continue;
        const size_t block = block_size_for_bitrate(out.params.bitrate);
        send_.blocks_expired += out.queue.size() / block;
        out.queue.clear();
        if (expiry_reported_) continue;  // once per start; the trace summary counts every one
        expiry_reported_ = true;
        char line[160];
        std::snprintf(line, sizeof line, "discarded voice the game did not read within %u ms", options_.stale_output_ms);
        note(line);
    }
}

void Service::summary(uint64_t t, bool force) {
    if (!options_.trace || (!force && t < next_summary_)) return;
    next_summary_ = t + 5000;
    char fields[1024];
    std::snprintf(fields, sizeof fields,
                  "\"frames\":%llu,\"blocks\":%llu,\"blocks_dropped\":%llu,\"read_calls\":%llu,\"bytes_read\":%llu,"
                  "\"write_calls\":%llu,\"bytes_written\":%llu,\"received_blocks\":%llu,\"pattern_ok\":%llu,"
                  "\"pattern_bad\":%llu,\"sequence_gaps\":%llu,\"skipped_bytes\":%llu,\"frames_decoded\":%llu,"
                  "\"frames_concealed\":%llu,\"decode_errors\":%llu,\"samples_played\":%llu,\"samples_dropped\":%llu,"
                  "\"underruns\":%llu,\"blocks_expired\":%llu",
                  (unsigned long long)send_.frames, (unsigned long long)send_.blocks, (unsigned long long)send_.blocks_dropped,
                  (unsigned long long)send_.read_calls, (unsigned long long)send_.bytes_read,
                  (unsigned long long)receive_.write_calls, (unsigned long long)receive_.bytes_written,
                  (unsigned long long)receive_.blocks, (unsigned long long)receive_.pattern_ok,
                  (unsigned long long)receive_.pattern_bad, (unsigned long long)receive_.sequence_gaps,
                  (unsigned long long)receive_.skipped_bytes, (unsigned long long)receive_.frames_decoded,
                  (unsigned long long)receive_.frames_concealed, (unsigned long long)receive_.decode_errors,
                  (unsigned long long)receive_.samples_played, (unsigned long long)receive_.samples_dropped,
                  (unsigned long long)receive_.underruns, (unsigned long long)send_.blocks_expired);
    options_.trace->event(t - epoch_, "summary", fields);
}

SendStats Service::send_stats() {
    std::lock_guard<std::mutex> hold(lock_);
    return send_;
}

ReceiveStats Service::receive_stats() {
    std::lock_guard<std::mutex> hold(lock_);
    return receive_;
}

void Service::trace_event(const char* name, const char* fields) {
    if (options_.trace) options_.trace->event(trace_ms(), name, fields);
}

}  // namespace mag::voice
