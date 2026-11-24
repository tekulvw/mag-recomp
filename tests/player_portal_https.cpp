// Drives the platform HTTPS transport (player_portal_openssl.cpp on Linux) once:
//   player_portal_https [GET|POST|PUT|DELETE] <url> <json body> [header value for X-Test]
// Without a method it posts. Prints the HTTP status and body, or "error: <why>" with exit status 1.
#include "player_portal.h"
#include <iostream>
#include <string>

int main(int argc, char **argv) {
    std::string method = "POST";
    if (argc > 1) {
        const std::string first = argv[1];
        if (first == "GET" || first == "POST" || first == "PUT" || first == "DELETE") { method = first; argv++; argc--; }
    }
    if (argc < 3) return 2;
    auto transport = mag::portal::make_platform_transport();
    mag::portal::Headers headers;
    if (argc > 3) headers.push_back({"X-Test", argv[3]});
    auto response = method == "POST" ? transport->post(argv[1], argv[2], headers) : transport->send(method, argv[1], argv[2], headers);
    if (!response.status) { std::cout << "error: " << response.error << '\n'; return 1; }
    std::cout << response.status << '\n' << response.body;
    return 0;
}
