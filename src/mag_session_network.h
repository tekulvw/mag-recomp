#pragma once
/* Network health for the session monitor (docs/player-settings.md, "Frame-rate dips and network
 * gaps").
 *
 * The runtime's socket observer (ps3emu/net_host_observer.h) reports each guest socket call that
 * opened, closed or moved data. Network::observe() turns those calls into counters per remote
 * endpoint: packets and bytes each way, a histogram of the gaps between arriving packets, blocked
 * and slow sends, socket errors, and the silences of endpoints that were streaming (receiving
 * steadily, as the game server's match traffic does). A silence of gap_event_us or more on a
 * streaming server endpoint is a network gap, the event the summary's timeline sets beside the
 * frame-rate dips. Nothing is logged per packet, and nothing here knows the game's protocol.
 *
 * The summary writer samples each open TCP server connection's round-trip time once a second
 * (tcp_round_trip_ms()) and closes a minute of network counters when the monitor closes a minute
 * of frames. The class takes its clock from its callers, so the tests drive it with made-up
 * times. */
#include <array>
#include <cstdint>
#include <mutex>
#include <string>
#include <utility>
#include <vector>

namespace mag::session::net {

/* ---- Thresholds (documented in docs/player-settings.md) ---- */
/* A silence this long on a streaming server endpoint is a network gap. */
constexpr uint64_t gap_event_us = 500000;
/* Streaming: the endpoint's last stream_packets packets arrived within stream_window_us. */
constexpr size_t stream_packets = 10;
constexpr uint64_t stream_window_us = 5000000;
/* A send call this long is a slow send. */
constexpr uint64_t slow_send_us = 50000;
/* Endpoints kept by name; more are counted with the other peers. */
constexpr size_t max_endpoints = 16;
/* Server addresses remembered (the addresses the game opened TCP connections to). */
constexpr size_t max_servers = 16;
/* Network gaps kept for the timeline (the latest). */
constexpr size_t max_gaps = 64;
/* Socket errors counted by name per endpoint. */
constexpr size_t max_error_kinds = 8;
/* The inter-arrival gap histogram's lower edges, in milliseconds; the last bucket is open. */
constexpr size_t gap_bucket_count = 8;
constexpr uint32_t gap_bucket_ms[gap_bucket_count] = {0, 50, 100, 200, 500, 1000, 2000, 5000};
size_t gap_bucket_of(uint64_t us);

enum class Kind { Connect = 1, Accept = 2, Close = 3, Send = 4, Recv = 5 };
enum class Transport { Unknown = 0, Stream = 1, Datagram = 2 };

/* libnet errno values (BSD numbering) the monitor treats specially. */
constexpr int32_t errno_would_block = 35, errno_in_progress = 36;
const char *errno_name(int32_t error);  /* "ECONNRESET", or "" for numbers it does not name */

/* One socket call, as the runtime reported it. */
struct Event {
    Kind kind = Kind::Send;
    int32_t socket = 0;
    Transport transport = Transport::Unknown;
    int64_t host_socket = -1;
    std::array<uint8_t, 4> address{};  /* network order; all zero when the call named no peer */
    uint16_t port = 0;
    int32_t bytes = 0;   /* -1 when the call failed */
    int32_t error = 0;   /* libnet errno, 0 on success */
    uint64_t elapsed_ns = 0;
};

/* A network gap: a silence of gap_event_us or more on a streaming server endpoint. */
struct Gap {
    double at_s = 0;  /* when the silence began (the last packet), seconds since the monitor started */
    double ms = 0;    /* its length, or its length so far when open */
    int phase = 0;    /* the phase when it began */
    int endpoint = -1;
    bool open = false;  /* not over when the snapshot was taken */
};

struct Direction {
    uint64_t packets = 0, bytes = 0;
};

struct EndpointStats {
    Transport transport = Transport::Unknown;
    std::array<uint8_t, 4> address{};
    uint16_t port = 0;
    bool server = false;  /* the game opened a TCP connection to this address */
    bool open = false;    /* a socket of the game is attached to it */
    double first_s = 0, last_s = 0;  /* its first and latest activity */
    Direction in, out;
    std::array<uint64_t, gap_bucket_count> gaps{};
    bool streamed = false;         /* streaming at some point */
    double longest_gap_ms = 0;     /* the longest silence that began while streaming */
    uint64_t gap_events = 0;
    uint64_t send_blocked = 0, send_slow = 0, peer_closed = 0;
    std::vector<std::pair<int32_t, uint64_t>> errors;  /* libnet errno, count */
    uint64_t rtt_samples = 0;
    double rtt_last_ms = 0, rtt_min_ms = 0, rtt_max_ms = 0, rtt_sum_ms = 0;
};

/* What the endpoints outside the named ones moved: peers that are not servers (their addresses
 * are never reported) and endpoints beyond max_endpoints. */
struct OtherStats {
    uint64_t endpoints = 0;  /* distinct peers, counted up to 256 */
    Direction in, out;
    uint64_t errors = 0;
};

/* One minute of network counters, closed with the monitor's minute. */
struct Minute {
    bool any = false;     /* some socket activity, or an open endpoint */
    bool stream = false;  /* some server endpoint was streaming */
    Direction in, out;
    double longest_gap_ms = 0;  /* the longest streaming silence that ended in the minute or was open at its end */
    uint64_t gap_events = 0, errors = 0, send_blocked = 0;
    double rtt_ms = 0;  /* the highest TCP round-trip time sampled in the minute; 0 without a sample */
};

struct Snapshot {
    bool available = false;  /* the runtime reports socket calls */
    std::vector<EndpointStats> endpoints;
    OtherStats other;
    std::vector<Gap> gaps;  /* the latest, oldest first, with any open silence last */
};

class Network {
public:
    explicit Network(uint64_t start_ns = 0) : start_ns_(start_ns) {}
    void set_available() { std::lock_guard<std::mutex> lock(mutex_); available_ = true; }
    /* One socket call at `now_ns`, in `phase` (the session phase). */
    void observe(const Event &, uint64_t now_ns, int phase);
    /* The open TCP server connections: (endpoint index, host socket), for round-trip sampling. */
    std::vector<std::pair<int, int64_t>> tcp_connections();
    void round_trip(int endpoint, double ms);
    /* The counters of the minute since the previous call (or the start), and starts the next. */
    Minute close_minute(uint64_t now_ns);
    Snapshot snapshot(uint64_t now_ns);
    /* One line for the stall reporter: each open server endpoint and how long ago its last packet
     * arrived ("tcp 192.0.2.1:10473 last received 12.3 s ago (was streaming)"). Never waits: "busy"
     * when a socket call holds the counters; empty when the runtime reports no socket calls. */
    std::string status(uint64_t now_ns);

private:
    struct Endpoint {
        EndpointStats stats;
        uint64_t last_rx_us = 0;
        int last_rx_phase = 0;
        bool streaming = false;  /* at the latest arrival */
        std::array<uint64_t, stream_packets> arrivals{};  /* a ring of arrival times */
        size_t arrival_count = 0;
        int sockets = 0;
        int64_t host_socket = -1;
    };
    int endpoint_for(Transport, const std::array<uint8_t, 4> &, uint16_t, uint64_t now_us);
    bool is_server(const std::array<uint8_t, 4> &) const;
    void count_error(int endpoint, int32_t error);
    uint64_t us(uint64_t now_ns) const { return now_ns > start_ns_ ? (now_ns - start_ns_) / 1000 : 0; }

    uint64_t start_ns_;
    std::mutex mutex_;
    bool available_ = false;
    std::vector<Endpoint> endpoints_;
    std::vector<std::array<uint8_t, 4>> servers_;
    std::array<int, 256> sockets_{};  /* guest socket -> endpoint + 1 (0: none; -1: the others) */
    std::vector<std::pair<std::array<uint8_t, 4>, uint32_t>> others_;  /* distinct other peers (address, transport << 16 | port) */
    OtherStats other_;
    std::vector<Gap> gaps_;
    Minute minute_;
};

/* The smoothed round-trip time the operating system keeps for a connected TCP socket, in
 * milliseconds, or a negative number when it cannot tell (TCP_INFO on Linux, TCP_CONNECTION_INFO
 * on macOS, SIO_TCP_INFO on Windows). Read-only. */
double tcp_round_trip_ms(int64_t host_socket);

/* "tcp" or "udp" (or "unknown"). */
const char *transport_name(Transport);
/* "a.b.c.d:port". */
std::string address_text(const std::array<uint8_t, 4> &, uint16_t port);
}  // namespace mag::session::net
