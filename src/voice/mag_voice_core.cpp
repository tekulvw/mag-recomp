#include "mag_voice_core.h"

#include <algorithm>
#include <cmath>
#include <cstring>

namespace mag::voice {

constexpr size_t MaxPorts = 32;

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
    initialized_ = false;
    started_ = false;
    ports_.clear();
    return status::Ok;
}

uint32_t Service::start() {
    std::lock_guard<std::mutex> hold(lock_);
    if (!initialized_) return status::NotInitialized;
    if (!started_) {
        started_ = true;
        started_at_ = now();
        frames_done_ = 0;
    }
    return status::Ok;
}

uint32_t Service::stop() {
    std::lock_guard<std::mutex> hold(lock_);
    if (!initialized_) return status::NotInitialized;
    started_ = false;
    gate_ = false;
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
        if (params.type == uint32_t(PortType::InVoice))
            port.parser = std::make_unique<BlockParser>(block_size_for_bitrate(params.bitrate));
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

static uint32_t pcm_frame_bytes(const PortParams& p) {
    const uint32_t sample = (p.pcm_type <= 1 || p.pcm_type >= 4) ? 4 : 2;  // float and int32: 4, short: 2
    return FrameSamples * sample * std::max<uint32_t>(p.pcm_channels, 1);
}

uint32_t Service::port_info(uint32_t id, PortInfo& info) {
    std::lock_guard<std::mutex> hold(lock_);
    if (!initialized_) return status::NotInitialized;
    pump();
    Port* port = find(id);
    if (!port) return status::Topology;
    info.type = port->params.type;
    info.state = uint32_t(started_ ? PortState::Running : PortState::Idle);
    info.edges = port->edges;
    if (voice_type(port->params.type)) {
        info.frame_bytes = static_cast<uint32_t>(block_size_for_bitrate(port->params.bitrate));
        info.queued_bytes = static_cast<uint32_t>(port->parser ? port->parser->pending() : port->queue.size());
    } else if (port->params.type == uint32_t(PortType::InPcm) || port->params.type == uint32_t(PortType::OutPcm)) {
        info.frame_bytes = pcm_frame_bytes(port->params);
        info.queued_bytes = 0;
    } else {
        info.frame_bytes = 0;
        info.queued_bytes = 0;
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
    if (port->params.type == uint32_t(PortType::OutVoice)) {
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
        accepted = bytes;  // PCM input: accepted; mixing it is the host-audio stage's work
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
        if (port->have_sequence && block.sequence != static_cast<uint8_t>(port->last_sequence + 1)) ++receive_.sequence_gaps;
        port->have_sequence = true;
        port->last_sequence = block.sequence;
    }
    receive_.skipped_bytes += port->parser->skipped_bytes() - skipped_before;
    return status::Ok;
}

uint32_t Service::signal_state(uint32_t id, uint32_t attr, float& energy, uint32_t& vad, bool& is_float) {
    std::lock_guard<std::mutex> hold(lock_);
    if (!initialized_) return status::NotInitialized;
    pump();
    if (!find(id)) return status::Topology;
    if (attr == AttrEnergyLevel) {
        energy = gate_ ? energy_ : 0.0f;
        is_float = true;
        return status::Ok;
    }
    if (attr == AttrVad) {
        vad = gate_ ? 1 : 0;
        is_float = false;
        return status::Ok;
    }
    return status::DeviceNotPresent;  // as the runtime stub answers every attribute
}

void Service::pump() {
    if (!started_ || !options_.source) return;
    const uint64_t t = now();
    const uint64_t due = (t - started_at_) / FrameMs;
    if (due <= frames_done_) {
        summary(t);
        return;
    }
    if (due - frames_done_ > options_.max_catch_up) frames_done_ = due - options_.max_catch_up;  // skip a stall
    std::vector<int16_t> samples(FrameSamples);
    std::vector<uint8_t> payload, block;
    for (; frames_done_ < due; ++frames_done_) {
        const uint64_t index = frames_done_;
        gate_ = options_.source->frame(index, samples.data());
        double sum = 0;
        for (int16_t s : samples) sum += double(s) * double(s);
        energy_ = static_cast<float>(std::sqrt(sum / FrameSamples) / 32768.0);
        if (!gate_) continue;
        ++send_.frames;
        for (auto& out : ports_) {
            if (!out.used || out.params.type != uint32_t(PortType::OutVoice) || out.params.mute) continue;
            const bool fed = std::any_of(out.edges.begin(), out.edges.end(), [&](uint32_t input) {
                const Port* in = find(input);
                return in && in->params.type == uint32_t(PortType::InMic) && !in->params.mute;
            });
            if (!fed) continue;
            const size_t size = block_size_for_bitrate(out.params.bitrate);
            payload.assign(std::min<size_t>(size - BlockHeader, 255), 0);
            const size_t n = options_.encoder ? options_.encoder->encode(index, samples.data(), payload.data(), payload.size()) : 0;
            if (!n) continue;
            block.resize(size);
            write_block(static_cast<uint8_t>(index), payload.data(), n, size, block.data());
            out.queue.insert(out.queue.end(), block.begin(), block.end());
            ++send_.blocks;
            const size_t cap = std::max<size_t>(size, size * (options_.queue_ms / FrameMs));
            while (out.queue.size() > cap) {
                out.queue.erase(out.queue.begin(), out.queue.begin() + static_cast<std::ptrdiff_t>(size));
                ++send_.blocks_dropped;
            }
        }
    }
    summary(t);
}

void Service::summary(uint64_t t, bool force) {
    if (!options_.trace || (!force && t < next_summary_)) return;
    next_summary_ = t + 5000;
    char fields[512];
    std::snprintf(fields, sizeof fields,
                  "\"frames\":%llu,\"blocks\":%llu,\"blocks_dropped\":%llu,\"read_calls\":%llu,\"bytes_read\":%llu,"
                  "\"write_calls\":%llu,\"bytes_written\":%llu,\"received_blocks\":%llu,\"pattern_ok\":%llu,"
                  "\"pattern_bad\":%llu,\"sequence_gaps\":%llu,\"skipped_bytes\":%llu",
                  (unsigned long long)send_.frames, (unsigned long long)send_.blocks, (unsigned long long)send_.blocks_dropped,
                  (unsigned long long)send_.read_calls, (unsigned long long)send_.bytes_read,
                  (unsigned long long)receive_.write_calls, (unsigned long long)receive_.bytes_written,
                  (unsigned long long)receive_.blocks, (unsigned long long)receive_.pattern_ok,
                  (unsigned long long)receive_.pattern_bad, (unsigned long long)receive_.sequence_gaps,
                  (unsigned long long)receive_.skipped_bytes);
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
