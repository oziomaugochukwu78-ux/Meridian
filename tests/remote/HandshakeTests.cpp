#include <remote/WebSocketProtocol.h>

#include <catch2/catch_test_macros.hpp>

#include <string>

using namespace meridian::remote::ws;

namespace
{
std::string requestHead (const std::string& target = "/", const std::string& extraHeaders = "")
{
    return "GET " + target + " HTTP/1.1\r\n"
           "Host: 127.0.0.1:47800\r\n"
           "Upgrade: websocket\r\n"
           "Connection: keep-alive, Upgrade\r\n"
           "Sec-WebSocket-Key: dGhlIHNhbXBsZSBub25jZQ==\r\n"
           "Sec-WebSocket-Version: 13\r\n"
           + extraHeaders + "\r\n";
}

HandshakeResult evaluate (const std::string& head, const HandshakePolicy& policy = {})
{
    const auto request = parseHttpRequest (head);
    REQUIRE (request.has_value());
    return evaluateHandshake (*request, policy);
}

bool startsWith (const std::string& s, const std::string& prefix) { return s.rfind (prefix, 0) == 0; }
} // namespace

TEST_CASE ("A native client without Origin is upgraded", "[handshake]")
{
    const auto result = evaluate (requestHead());
    REQUIRE (result.accepted);
    REQUIRE (startsWith (result.response, "HTTP/1.1 101 Switching Protocols\r\n"));
    REQUIRE (result.response.find ("Sec-WebSocket-Accept: s3pPLMBiTxaQ9kYGzzhZRbK+xOo=\r\n") != std::string::npos);
}

TEST_CASE ("Header names are case-insensitive", "[handshake]")
{
    const std::string head = "GET / HTTP/1.1\r\n"
                             "UPGRADE: WebSocket\r\n"
                             "connection: upgrade\r\n"
                             "sec-websocket-key: dGhlIHNhbXBsZSBub25jZQ==\r\n"
                             "SEC-WEBSOCKET-VERSION: 13\r\n\r\n";
    REQUIRE (evaluate (head).accepted);
}

TEST_CASE ("Browser pages from unknown origins are refused", "[handshake][security]")
{
    const auto result = evaluate (requestHead ("/", "Origin: https://evil.example\r\n"));
    REQUIRE_FALSE (result.accepted);
    REQUIRE (startsWith (result.response, "HTTP/1.1 403"));

    HandshakePolicy policy;
    policy.allowedOrigins = { "http://localhost:3000" };
    REQUIRE (evaluate (requestHead ("/", "Origin: http://LOCALHOST:3000\r\n"), policy).accepted);
}

TEST_CASE ("Token is enforced when configured", "[handshake][security]")
{
    HandshakePolicy policy;
    policy.requiredToken = "s3cret token";

    REQUIRE (startsWith (evaluate (requestHead(), policy).response, "HTTP/1.1 401"));
    REQUIRE_FALSE (evaluate (requestHead ("/?token=wrong"), policy).accepted);
    REQUIRE (evaluate (requestHead ("/?token=s3cret%20token"), policy).accepted);
    REQUIRE (evaluate (requestHead ("/", "Authorization: Bearer s3cret token\r\n"), policy).accepted);
}

TEST_CASE ("Malformed or non-WebSocket requests are rejected", "[handshake]")
{
    REQUIRE_FALSE (evaluate ("POST / HTTP/1.1\r\nUpgrade: websocket\r\n\r\n").accepted);
    REQUIRE (startsWith (evaluate ("GET / HTTP/1.1\r\nHost: x\r\n\r\n").response, "HTTP/1.1 426"));

    std::string wrongVersion = requestHead();
    wrongVersion.replace (wrongVersion.find ("Version: 13"), 11, "Version: 8");
    const auto versionResult = evaluate (wrongVersion);
    REQUIRE_FALSE (versionResult.accepted);
    REQUIRE (versionResult.response.find ("Sec-WebSocket-Version: 13") != std::string::npos);

    REQUIRE_FALSE (parseHttpRequest ("garbage").has_value());
}
