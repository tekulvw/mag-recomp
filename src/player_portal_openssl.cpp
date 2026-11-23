// The portal's HTTPS transport on Linux: OpenSSL (libssl) over a plain socket.
//
// Why OpenSSL: the installer already requires libssl-dev and ca-certificates on Linux (the
// launcher links libcrypto), whereas neither libcurl nor the curl program is installed by
// default. Certificates are verified against the system store (SSL_CERT_FILE / SSL_CERT_DIR or
// OpenSSL's default paths), with the host name checked, TLS 1.2 or newer. Plain http is used only
// for the localhost portal address that check_portal_url allows for tests.
#include "player_portal.h"
#include <openssl/err.h>
#include <openssl/ssl.h>
#include <openssl/x509v3.h>
#include <algorithm>
#include <cctype>
#include <cerrno>
#include <cstdlib>
#include <csignal>
#include <cstring>
#include <ctime>
#include <fcntl.h>
#include <netdb.h>
#include <poll.h>
#include <pthread.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <unistd.h>

namespace mag::portal {
namespace {
constexpr int connect_seconds = 10, io_seconds = 20;
constexpr size_t reply_limit = 1 << 20;

std::string lower(std::string s) {
    for (auto &c : s) c = char(std::tolower((unsigned char)c));
    return s;
}

std::string ssl_error(const char *what) {
    unsigned long code = ERR_get_error();
    char text[256] = {0};
    if (code) ERR_error_string_n(code, text, sizeof text);
    ERR_clear_error();
    return std::string(what) + (code ? std::string(": ") + text : std::string());
}

// A writer to a closed connection would otherwise raise SIGPIPE and end the supervisor; block it
// on this thread for the request and consume any that arrived (the game's disposition is untouched).
class NoSigpipe {
    sigset_t old_{}, pipe_{};
    bool pending_before_ = false;
public:
    NoSigpipe() {
        sigemptyset(&pipe_); sigaddset(&pipe_, SIGPIPE);
        sigset_t pending; sigpending(&pending);
        pending_before_ = sigismember(&pending, SIGPIPE);
        pthread_sigmask(SIG_BLOCK, &pipe_, &old_);
    }
    ~NoSigpipe() {
        if (!pending_before_) {
            const timespec none{0, 0};
            while (sigtimedwait(&pipe_, nullptr, &none) > 0) {}
        }
        pthread_sigmask(SIG_SETMASK, &old_, nullptr);
    }
};

int connect_to(const Url &url, std::string &error) {
    addrinfo hints{}, *found = nullptr;
    hints.ai_family = AF_UNSPEC; hints.ai_socktype = SOCK_STREAM;
    int rc = getaddrinfo(url.host.c_str(), std::to_string(url.port).c_str(), &hints, &found);
    if (rc) { error = "cannot find " + url.host + " (" + gai_strerror(rc) + ")"; return -1; }
    int fd = -1;
    error = "cannot connect to " + url.host;
    for (addrinfo *a = found; a; a = a->ai_next) {
        fd = socket(a->ai_family, a->ai_socktype | SOCK_CLOEXEC | SOCK_NONBLOCK, a->ai_protocol);
        if (fd < 0) continue;
        if (connect(fd, a->ai_addr, a->ai_addrlen) == 0 || errno == EINPROGRESS) {
            pollfd p{fd, POLLOUT, 0};
            int soerr = 0; socklen_t len = sizeof soerr;
            if (poll(&p, 1, connect_seconds * 1000) == 1 && getsockopt(fd, SOL_SOCKET, SO_ERROR, &soerr, &len) == 0 && soerr == 0) break;
            error = "cannot connect to " + url.host + (soerr ? std::string(" (") + std::strerror(soerr) + ")" : std::string(" (timed out)"));
        }
        close(fd); fd = -1;
    }
    freeaddrinfo(found);
    if (fd < 0) return -1;
    // Blocking from here on, with timeouts, so OpenSSL can read and write simply.
    fcntl(fd, F_SETFL, fcntl(fd, F_GETFL) & ~O_NONBLOCK);
    timeval tv{io_seconds, 0};
    setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof tv);
    setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof tv);
    return fd;
}

// The status and body of an HTTP/1.1 reply (Content-Length, chunked, or read to close).
bool parse_reply(const std::string &raw, Response &out) {
    auto head_end = raw.find("\r\n\r\n");
    if (head_end == std::string::npos || raw.compare(0, 5, "HTTP/")) return false;
    auto space = raw.find(' ');
    if (space == std::string::npos || space + 4 > head_end) return false;
    int status = 0;
    for (size_t i = space + 1; i < space + 4; ++i) {
        if (!std::isdigit((unsigned char)raw[i])) return false;
        status = status * 10 + (raw[i] - '0');
    }
    bool chunked = false; long long length = -1;
    size_t line = raw.find("\r\n") + 2;
    while (line < head_end) {
        size_t next = raw.find("\r\n", line);
        std::string header = raw.substr(line, next - line);
        auto colon = header.find(':');
        if (colon != std::string::npos) {
            std::string name = lower(header.substr(0, colon)), value = header.substr(colon + 1);
            value.erase(0, value.find_first_not_of(" \t"));
            if (name == "transfer-encoding" && lower(value).find("chunked") != std::string::npos) chunked = true;
            if (name == "content-length") {
                char *end = nullptr; errno = 0;
                length = std::strtoll(value.c_str(), &end, 10);
                if (errno || end == value.c_str() || length < 0) return false;
            }
        }
        line = next + 2;
    }
    std::string body = raw.substr(head_end + 4);
    if (chunked) {
        std::string decoded; size_t at = 0;
        for (;;) {
            size_t eol = body.find("\r\n", at);
            if (eol == std::string::npos) return false;
            const std::string size_line = body.substr(at, eol - at);  // hex size, maybe ";extensions"
            char *end = nullptr;
            const unsigned long long size = std::strtoull(size_line.c_str(), &end, 16);
            if (end == size_line.c_str()) return false;
            at = eol + 2;
            if (size == 0) break;
            if (size > body.size() - at) return false;
            decoded += body.substr(at, size);
            at += size + 2;
            if (at > body.size()) return false;
        }
        body = decoded;
    } else if (length >= 0) {
        if (size_t(length) > body.size()) return false;
        body.resize(size_t(length));
    }
    out.status = status;
    out.body = body;
    return true;
}

class OpenSslTransport final : public Transport {
public:
    Response post(const std::string &url_text, const std::string &body, const Headers &headers) override {
        Response response;
        Url url;
        if (!split_url(url_text, url)) { response.error = "the portal address is not a valid URL"; return response; }
        NoSigpipe guard;
        std::string error;
        int fd = connect_to(url, error);
        if (fd < 0) { response.error = error; return response; }
        SSL_CTX *context = nullptr; SSL *ssl = nullptr;
        if (url.scheme == "https") {
            context = SSL_CTX_new(TLS_client_method());
            if (!context || !SSL_CTX_set_min_proto_version(context, TLS1_2_VERSION) || !SSL_CTX_set_default_verify_paths(context)) {
                response.error = ssl_error("TLS is unavailable");
                if (context) SSL_CTX_free(context);
                close(fd); return response;
            }
            SSL_CTX_set_verify(context, SSL_VERIFY_PEER, nullptr);
            ssl = SSL_new(context);
            if (!ssl || !SSL_set_tlsext_host_name(ssl, url.host.c_str()) || !SSL_set1_host(ssl, url.host.c_str()) ||
                !SSL_set_fd(ssl, fd) || SSL_connect(ssl) != 1) {
                long verify = ssl ? SSL_get_verify_result(ssl) : X509_V_OK;
                response.error = verify != X509_V_OK ? std::string("the portal's certificate was refused (") +
                                                           X509_verify_cert_error_string(verify) + ")"
                                                     : ssl_error("the secure connection failed");
                if (ssl) SSL_free(ssl);
                SSL_CTX_free(context); close(fd); return response;
            }
        }
        std::string request = "POST " + url.path + " HTTP/1.1\r\nHost: " + url.host +
                              (url.port == (url.scheme == "https" ? 443 : 80) ? std::string() : ":" + std::to_string(url.port)) +
                              "\r\nUser-Agent: MAG-launcher\r\nAccept: application/json\r\nContent-Type: application/json\r\n"
                              "Content-Length: " + std::to_string(body.size()) + "\r\nConnection: close\r\n";
        for (const auto &[name, value] : headers) request += name + ": " + value + "\r\n";
        request += "\r\n" + body;
        bool sent = true;
        for (size_t done = 0; sent && done < request.size();) {
            const int chunk = int(std::min<size_t>(request.size() - done, 1 << 16));
            const long n = ssl ? SSL_write(ssl, request.data() + done, chunk) : send(fd, request.data() + done, size_t(chunk), MSG_NOSIGNAL);
            if (n < 0 && !ssl && errno == EINTR) continue;
            if (n <= 0) sent = false; else done += size_t(n);
        }
        std::string raw;
        if (sent) {
            char buffer[16384];
            for (;;) {
                long n = ssl ? SSL_read(ssl, buffer, sizeof buffer) : recv(fd, buffer, sizeof buffer, 0);
                if (n < 0 && !ssl && errno == EINTR) continue;
                if (n <= 0) break;  // the server closes the connection after the reply (Connection: close)
                raw.append(buffer, size_t(n));
                if (raw.size() > reply_limit) break;
            }
        }
        if (ssl) { SSL_shutdown(ssl); SSL_free(ssl); }
        if (context) SSL_CTX_free(context);
        close(fd);
        ERR_clear_error();
        if (!sent) response.error = "the connection to " + url.host + " was lost";
        else if (raw.size() > reply_limit) response.error = "the reply from " + url.host + " was too large";
        else if (!parse_reply(raw, response)) response.error = raw.empty() ? "no reply from " + url.host + " (timed out or closed)"
                                                                           : "an incomplete reply from " + url.host;
        if (!response.error.empty()) { response.status = 0; response.body.clear(); }
        return response;
    }
};
}  // namespace

std::unique_ptr<Transport> make_platform_transport() { return std::make_unique<OpenSslTransport>(); }
}  // namespace mag::portal
