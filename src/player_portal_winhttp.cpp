// The portal's HTTPS transport on Windows: WinHTTP, which verifies the certificate chain and the
// host name against the Windows certificate store by default (no security flags are relaxed).
// Redirects are refused so the device token never follows one; the system proxy is honoured.
#include "player_portal.h"
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <winhttp.h>
#include <algorithm>
#include <string>

#ifndef WINHTTP_FLAG_SECURE_PROTOCOL_TLS1_3
#define WINHTTP_FLAG_SECURE_PROTOCOL_TLS1_3 0x00002000
#endif
#ifndef WINHTTP_ACCESS_TYPE_AUTOMATIC_PROXY
#define WINHTTP_ACCESS_TYPE_AUTOMATIC_PROXY 4
#endif

namespace mag::portal {
namespace {
std::wstring wide(const std::string &s) {
    if (s.empty()) return {};
    int n = MultiByteToWideChar(CP_UTF8, 0, s.data(), int(s.size()), nullptr, 0);
    std::wstring out(size_t(n > 0 ? n : 0), L'\0');
    if (n > 0) MultiByteToWideChar(CP_UTF8, 0, s.data(), int(s.size()), out.data(), n);
    return out;
}

std::string describe(DWORD code) {
    switch (code) {
    case ERROR_WINHTTP_SECURE_FAILURE: case ERROR_WINHTTP_SECURE_INVALID_CA: case ERROR_WINHTTP_SECURE_CERT_CN_INVALID:
    case ERROR_WINHTTP_SECURE_CERT_DATE_INVALID: case ERROR_WINHTTP_SECURE_INVALID_CERT: case ERROR_WINHTTP_SECURE_CERT_REVOKED:
        return "the portal's certificate was refused";
    case ERROR_WINHTTP_NAME_NOT_RESOLVED: return "the portal's name could not be found";
    case ERROR_WINHTTP_CANNOT_CONNECT: return "cannot connect";
    case ERROR_WINHTTP_TIMEOUT: return "no reply (timed out)";
    case ERROR_WINHTTP_CONNECTION_ERROR: return "the connection was lost";
    }
    return "network error " + std::to_string(code);
}

struct Handle {
    HINTERNET h = nullptr;
    ~Handle() { if (h) WinHttpCloseHandle(h); }
};

class WinHttpTransport final : public Transport {
public:
    Response post(const std::string &url_text, const std::string &body, const Headers &headers) override {
        Response response;
        Url url;
        if (!split_url(url_text, url)) { response.error = "the portal address is not a valid URL"; return response; }
        auto failed = [&] { response.status = 0; response.body.clear(); response.error = describe(GetLastError()); return response; };
        Handle session{WinHttpOpen(L"MAG-launcher", WINHTTP_ACCESS_TYPE_AUTOMATIC_PROXY, WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, 0)};
        if (!session.h)  // before Windows 8.1
            session.h = WinHttpOpen(L"MAG-launcher", WINHTTP_ACCESS_TYPE_DEFAULT_PROXY, WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, 0);
        if (!session.h) return failed();
        WinHttpSetTimeouts(session.h, 10000, 10000, 20000, 20000);
        DWORD protocols = WINHTTP_FLAG_SECURE_PROTOCOL_TLS1_2 | WINHTTP_FLAG_SECURE_PROTOCOL_TLS1_3;
        if (!WinHttpSetOption(session.h, WINHTTP_OPTION_SECURE_PROTOCOLS, &protocols, sizeof protocols)) {
            protocols = WINHTTP_FLAG_SECURE_PROTOCOL_TLS1_2;  // Windows without TLS 1.3
            WinHttpSetOption(session.h, WINHTTP_OPTION_SECURE_PROTOCOLS, &protocols, sizeof protocols);
        }
        Handle connection{WinHttpConnect(session.h, wide(url.host).c_str(), INTERNET_PORT(url.port), 0)};
        if (!connection.h) return failed();
        Handle request{WinHttpOpenRequest(connection.h, L"POST", wide(url.path).c_str(), nullptr, WINHTTP_NO_REFERER,
                                          WINHTTP_DEFAULT_ACCEPT_TYPES, url.scheme == "https" ? WINHTTP_FLAG_SECURE : 0)};
        if (!request.h) return failed();
        DWORD never = WINHTTP_OPTION_REDIRECT_POLICY_NEVER;
        WinHttpSetOption(request.h, WINHTTP_OPTION_REDIRECT_POLICY, &never, sizeof never);
        std::string extra = "Content-Type: application/json\r\nAccept: application/json\r\n";
        for (const auto &[name, value] : headers) extra += name + ": " + value + "\r\n";
        const std::wstring header_text = wide(extra);
        if (!WinHttpSendRequest(request.h, header_text.c_str(), DWORD(header_text.size()), (LPVOID)body.data(), DWORD(body.size()),
                                DWORD(body.size()), 0) ||
            !WinHttpReceiveResponse(request.h, nullptr))
            return failed();
        DWORD status = 0, size = sizeof status;
        if (!WinHttpQueryHeaders(request.h, WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER, WINHTTP_HEADER_NAME_BY_INDEX,
                                 &status, &size, WINHTTP_NO_HEADER_INDEX))
            return failed();
        wchar_t retry[32]; DWORD retry_size = sizeof retry;
        if (WinHttpQueryHeaders(request.h, WINHTTP_QUERY_RETRY_AFTER, WINHTTP_HEADER_NAME_BY_INDEX, retry, &retry_size,
                                WINHTTP_NO_HEADER_INDEX)) {
            std::string value;
            for (DWORD i = 0; i < retry_size / sizeof(wchar_t) && retry[i]; ++i) value += retry[i] < 128 ? char(retry[i]) : '?';
            response.retry_after = parse_retry_after(value);
        }
        for (;;) {
            DWORD available = 0, read = 0;
            if (!WinHttpQueryDataAvailable(request.h, &available)) return failed();
            if (!available) break;
            const size_t start = response.body.size();
            response.body.resize(start + available);
            if (!WinHttpReadData(request.h, response.body.data() + start, available, &read)) return failed();
            response.body.resize(start + read);
            if (response.body.size() > (1u << 20)) { response.body.clear(); response.error = "the reply was too large"; return response; }
        }
        response.status = int(status);
        return response;
    }
};
}  // namespace

std::unique_ptr<Transport> make_platform_transport() { return std::make_unique<WinHttpTransport>(); }
}  // namespace mag::portal
