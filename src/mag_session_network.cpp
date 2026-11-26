// Network health for the session monitor (src/mag_session_network.h).
#include "mag_session_network.h"

#include <algorithm>
#include <cstdio>

#if defined(_WIN32)
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <winsock2.h>
#include <ws2tcpip.h>
#include <mstcpip.h>
#if defined(_MSC_VER)
#pragma comment(lib, "ws2_32.lib")
#endif
#else
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <sys/socket.h>
#endif

namespace mag::session::net {

size_t gap_bucket_of(uint64_t us) {
    size_t b = 0;
    while (b + 1 < gap_bucket_count && us >= uint64_t(gap_bucket_ms[b + 1]) * 1000) ++b;
    return b;
}

const char *errno_name(int32_t error) {
    switch (error) {
    case 9: return "EBADF";
    case 12: return "ENOMEM";
    case 22: return "EINVAL";
    case 32: return "EPIPE";
    case 35: return "EWOULDBLOCK";
    case 36: return "EINPROGRESS";
    case 37: return "EALREADY";
    case 40: return "EMSGSIZE";
    case 48: return "EADDRINUSE";
    case 49: return "EADDRNOTAVAIL";
    case 50: return "ENETDOWN";
    case 51: return "ENETUNREACH";
    case 53: return "ECONNABORTED";
    case 54: return "ECONNRESET";
    case 57: return "ENOTCONN";
    case 60: return "ETIMEDOUT";
    case 61: return "ECONNREFUSED";
    case 64: return "EHOSTDOWN";
    case 65: return "EHOSTUNREACH";
    default: return "";
    }
}

const char *transport_name(Transport t) {
    return t == Transport::Stream ? "tcp" : t == Transport::Datagram ? "udp" : "unknown";
}

std::string address_text(const std::array<uint8_t, 4> &a, uint16_t port) {
    char text[32];
    std::snprintf(text, sizeof text, "%u.%u.%u.%u:%u", a[0], a[1], a[2], a[3], unsigned(port));
    return text;
}

namespace {
bool named(const std::array<uint8_t, 4> &a, uint16_t port) { return port || a[0] || a[1] || a[2] || a[3]; }
}  // namespace

bool Network::is_server(const std::array<uint8_t, 4> &a) const {
    return std::find(servers_.begin(), servers_.end(), a) != servers_.end();
}

// The endpoint for a peer: a named one (index), or -1 for the others (not a server, or no room).
int Network::endpoint_for(Transport transport, const std::array<uint8_t, 4> &address, uint16_t port, uint64_t now_us) {
    for (size_t i = 0; i < endpoints_.size(); ++i) {
        const EndpointStats &s = endpoints_[i].stats;
        if (s.transport == transport && s.address == address && s.port == port) return int(i);
    }
    if (!is_server(address) || endpoints_.size() >= max_endpoints) {
        const std::pair<std::array<uint8_t, 4>, uint32_t> key{address, uint32_t(transport) << 16 | port};
        if (others_.size() < 256 && std::find(others_.begin(), others_.end(), key) == others_.end()) {
            others_.push_back(key);
            ++other_.endpoints;
        }
        return -1;
    }
    Endpoint e;
    e.stats.transport = transport;
    e.stats.address = address;
    e.stats.port = port;
    e.stats.server = true;
    e.stats.first_s = e.stats.last_s = double(now_us) / 1e6;
    endpoints_.push_back(e);
    return int(endpoints_.size() - 1);
}

void Network::count_error(int endpoint, int32_t error) {
    ++minute_.errors;
    if (endpoint < 0) { ++other_.errors; return; }
    auto &errors = endpoints_[size_t(endpoint)].stats.errors;
    for (auto &[code, count] : errors)
        if (code == error) { ++count; return; }
    if (errors.size() < max_error_kinds) errors.emplace_back(error, 1);
    else ++errors.back().second;
}

void Network::observe(const Event &e, uint64_t now_ns, int phase) {
    const uint64_t now_us = us(now_ns);
    std::lock_guard<std::mutex> lock(mutex_);
    available_ = true;
    minute_.any = true;
    int none = 0;
    const bool in_table = e.socket > 0 && size_t(e.socket) < sockets_.size();
    int &slot = in_table ? sockets_[size_t(e.socket)] : none;
    const bool peer = named(e.address, e.port);
    // A TCP connection the game opened names a server: the address is reported, and UDP traffic
    // with the same address belongs to the same server.
    if (e.kind == Kind::Connect && e.transport == Transport::Stream && peer && !is_server(e.address) && servers_.size() < max_servers)
        servers_.push_back(e.address);
    int endpoint = -2;  // -2: no endpoint
    if (peer && e.kind != Kind::Close) {
        const Transport transport = e.transport == Transport::Unknown ? Transport::Datagram : e.transport;
        endpoint = endpoint_for(transport, e.address, e.port, now_us);
    } else if (slot) {
        endpoint = slot > 0 ? slot - 1 : -1;
    }
    Endpoint *ep = endpoint >= 0 ? &endpoints_[size_t(endpoint)] : nullptr;
    if (ep) ep->stats.last_s = double(now_us) / 1e6;
    // Detaches the socket from the endpoint it was attached to.
    auto detach = [&]() {
        if (slot > 0) {
            Endpoint &previous = endpoints_[size_t(slot - 1)];
            if (--previous.sockets <= 0) {
                previous.sockets = 0;
                previous.stats.open = false;
            }
        }
        slot = 0;
    };
    switch (e.kind) {
    case Kind::Connect:
    case Kind::Accept:
        detach();
        if (e.error && e.error != errno_in_progress) {
            count_error(endpoint == -2 ? -1 : endpoint, e.error);  // a failed connect leaves the socket unattached
            return;
        }
        if (endpoint == -2) return;
        slot = endpoint >= 0 ? endpoint + 1 : -1;
        if (ep) {
            ++ep->sockets;
            ep->stats.open = true;
            ep->host_socket = e.host_socket;
        }
        return;
    case Kind::Close:
        detach();
        return;
    case Kind::Send:
    case Kind::Recv:
        break;
    }
    if (endpoint == -2) {
        // Data on a socket that never named its peer: counted with the others.
        endpoint = -1;
    } else if (in_table && !slot) {
        // A datagram socket's first send or receive attaches it to that peer.
        slot = endpoint >= 0 ? endpoint + 1 : -1;
        if (ep) {
            ++ep->sockets;
            ep->stats.open = true;
            ep->host_socket = e.host_socket;
        }
    }
    if (e.kind == Kind::Send) {
        if (e.bytes > 0) {
            Direction &d = ep ? ep->stats.out : other_.out;
            ++d.packets;
            d.bytes += uint64_t(e.bytes);
            ++minute_.out.packets;
            minute_.out.bytes += uint64_t(e.bytes);
        }
        if (e.error == errno_would_block) {
            if (ep) ++ep->stats.send_blocked;
            ++minute_.send_blocked;
        } else if (e.error) {
            count_error(endpoint, e.error);
        }
        if (ep && e.elapsed_ns >= slow_send_us * 1000) ++ep->stats.send_slow;
        return;
    }
    // A receive.
    if (e.error) { count_error(endpoint, e.error); return; }
    if (e.bytes == 0) {
        if (ep && ep->stats.transport == Transport::Stream) ++ep->stats.peer_closed;
        return;
    }
    ++minute_.in.packets;
    minute_.in.bytes += uint64_t(e.bytes);
    if (!ep) {
        ++other_.in.packets;
        other_.in.bytes += uint64_t(e.bytes);
        return;
    }
    EndpointStats &s = ep->stats;
    ++s.in.packets;
    s.in.bytes += uint64_t(e.bytes);
    if (ep->last_rx_us && now_us >= ep->last_rx_us) {
        const uint64_t gap = now_us - ep->last_rx_us;
        ++s.gaps[gap_bucket_of(gap)];
        if (ep->streaming) {
            // A silence that began while the endpoint was streaming.
            s.longest_gap_ms = std::max(s.longest_gap_ms, double(gap) / 1000.0);
            minute_.longest_gap_ms = std::max(minute_.longest_gap_ms, double(gap) / 1000.0);
            if (gap >= gap_event_us) {
                ++s.gap_events;
                ++minute_.gap_events;
                gaps_.push_back(Gap{double(ep->last_rx_us) / 1e6, double(gap) / 1000.0, ep->last_rx_phase, endpoint, false});
                if (gaps_.size() > max_gaps) gaps_.erase(gaps_.begin());
            }
        }
    }
    ep->arrivals[ep->arrival_count % stream_packets] = now_us;
    ++ep->arrival_count;
    // The oldest of the last stream_packets arrivals sits where the next one will go.
    ep->streaming = ep->arrival_count >= stream_packets &&
                    now_us - ep->arrivals[ep->arrival_count % stream_packets] <= stream_window_us;
    if (ep->streaming) s.streamed = true;
    if (ep->streaming) minute_.stream = true;
    ep->last_rx_us = now_us;
    ep->last_rx_phase = phase;
}

std::vector<std::pair<int, int64_t>> Network::tcp_connections() {
    std::lock_guard<std::mutex> lock(mutex_);
    std::vector<std::pair<int, int64_t>> out;
    for (size_t i = 0; i < endpoints_.size(); ++i) {
        const Endpoint &e = endpoints_[i];
        if (e.stats.open && e.stats.transport == Transport::Stream && e.host_socket >= 0) out.emplace_back(int(i), e.host_socket);
    }
    return out;
}

void Network::round_trip(int endpoint, double ms) {
    if (ms < 0) return;
    std::lock_guard<std::mutex> lock(mutex_);
    if (endpoint < 0 || size_t(endpoint) >= endpoints_.size()) return;
    EndpointStats &s = endpoints_[size_t(endpoint)].stats;
    s.rtt_min_ms = s.rtt_samples ? std::min(s.rtt_min_ms, ms) : ms;
    s.rtt_max_ms = s.rtt_samples ? std::max(s.rtt_max_ms, ms) : ms;
    s.rtt_last_ms = ms;
    s.rtt_sum_ms += ms;
    ++s.rtt_samples;
    minute_.rtt_ms = std::max(minute_.rtt_ms, ms);
}

Minute Network::close_minute(uint64_t now_ns) {
    const uint64_t now_us = us(now_ns);
    std::lock_guard<std::mutex> lock(mutex_);
    Minute m = minute_;
    // A silence still open at the minute's end counts in it (and again in the next if it goes on).
    for (const Endpoint &e : endpoints_) {
        if (!e.stats.open) continue;
        m.any = true;
        if (e.streaming && e.last_rx_us && now_us > e.last_rx_us) {
            m.stream = true;
            m.longest_gap_ms = std::max(m.longest_gap_ms, double(now_us - e.last_rx_us) / 1000.0);
        }
    }
    minute_ = Minute{};
    return m;
}

Snapshot Network::snapshot(uint64_t now_ns) {
    const uint64_t now_us = us(now_ns);
    std::lock_guard<std::mutex> lock(mutex_);
    Snapshot s;
    s.available = available_;
    s.other = other_;
    s.gaps = gaps_;
    for (size_t i = 0; i < endpoints_.size(); ++i) {
        const Endpoint &e = endpoints_[i];
        s.endpoints.push_back(e.stats);
        // The silence now, on a streaming endpoint that is still attached.
        if (e.stats.open && e.streaming && e.last_rx_us && now_us >= e.last_rx_us + gap_event_us) {
            const double ms = double(now_us - e.last_rx_us) / 1000.0;
            s.gaps.push_back(Gap{double(e.last_rx_us) / 1e6, ms, e.last_rx_phase, int(i), true});
            s.endpoints.back().longest_gap_ms = std::max(s.endpoints.back().longest_gap_ms, ms);
        }
    }
    if (s.gaps.size() > max_gaps) s.gaps.erase(s.gaps.begin(), s.gaps.end() - std::ptrdiff_t(max_gaps));
    return s;
}

std::string Network::status(uint64_t now_ns) {
    const uint64_t now_us = us(now_ns);
    std::unique_lock<std::mutex> lock(mutex_, std::try_to_lock);
    if (!lock.owns_lock()) return "busy";
    if (!available_) return {};
    std::string line;
    for (const Endpoint &e : endpoints_) {
        if (!e.stats.open) continue;
        if (!line.empty()) line += "; ";
        line += std::string(transport_name(e.stats.transport)) + " " + address_text(e.stats.address, e.stats.port);
        if (!e.last_rx_us) { line += " nothing received"; continue; }
        char text[64];
        std::snprintf(text, sizeof text, " last received %.1f s ago", double(now_us > e.last_rx_us ? now_us - e.last_rx_us : 0) / 1e6);
        line += text;
        if (e.streaming) line += " (was streaming)";
    }
    return line.empty() ? "no open server connection" : line;
}

double tcp_round_trip_ms(int64_t host_socket) {
    if (host_socket < 0) return -1;
#if defined(_WIN32)
#if defined(SIO_TCP_INFO)
    DWORD version = 0, returned = 0;
    TCP_INFO_v0 info{};
    if (WSAIoctl(SOCKET(host_socket), SIO_TCP_INFO, &version, sizeof version, &info, sizeof info, &returned, nullptr, nullptr) == 0)
        return double(info.RttUs) / 1000.0;
#endif
    return -1;
#elif defined(__APPLE__) && defined(TCP_CONNECTION_INFO)
    struct tcp_connection_info info {};
    socklen_t length = sizeof info;
    if (getsockopt(int(host_socket), IPPROTO_TCP, TCP_CONNECTION_INFO, &info, &length) == 0 && info.tcpi_srtt)
        return double(info.tcpi_srtt);  // milliseconds
    return -1;
#elif defined(TCP_INFO)
    struct tcp_info info {};
    socklen_t length = sizeof info;
    if (getsockopt(int(host_socket), IPPROTO_TCP, TCP_INFO, &info, &length) == 0 && info.tcpi_rtt)
        return double(info.tcpi_rtt) / 1000.0;  // microseconds
    return -1;
#else
    return -1;
#endif
}
}  // namespace mag::session::net
