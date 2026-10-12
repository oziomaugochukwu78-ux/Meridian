#pragma once

// RFC 6455 WebSocket protocol logic, independent of any socket API.
// The JUCE-based server (src/remote/WebSocketServer) only moves bytes; every
// protocol decision is made here, where it is unit-tested.

#include <cstddef>
#include <cstdint>
#include <deque>
#include <map>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace meridian::remote::ws
{

enum class Opcode : uint8_t
{
    continuation = 0x0,
    text = 0x1,
    binary = 0x2,
    close = 0x8,
    ping = 0x9,
    pong = 0xA,
};

namespace closeCode
{
    constexpr uint16_t normal = 1000;
    constexpr uint16_t goingAway = 1001;
    constexpr uint16_t protocolError = 1002;
    constexpr uint16_t unsupportedData = 1003;
    constexpr uint16_t invalidPayload = 1007;
    constexpr uint16_t policyViolation = 1008;
    constexpr uint16_t messageTooBig = 1009;
} // namespace closeCode

// ---------------------------------------------------------------------------
// Opening handshake
// ---------------------------------------------------------------------------

struct HttpRequest
{
    std::string method;
    std::string target;   ///< Request target including any query, e.g. "/?token=abc"
    std::string version;
    std::map<std::string, std::string> headers; ///< Keys lower-cased, values trimmed.

    std::string header (const std::string& lowerCaseName) const;
};

/** Parses an HTTP/1.1 request head (everything up to and including the blank line). */
std::optional<HttpRequest> parseHttpRequest (std::string_view head);

/** Sec-WebSocket-Accept value for a client's Sec-WebSocket-Key. */
std::string computeAcceptKey (std::string_view clientKey);

struct HandshakePolicy
{
    /** Browsers attach an Origin header to every WebSocket request, and any web
        page can open a socket to localhost. A request with an Origin not listed
        here is refused, so a malicious page cannot drive the DAW. Native
        clients (AI agents, scripts) send no Origin and are accepted. */
    std::vector<std::string> allowedOrigins;

    /** When non-empty, clients must present this token, either as
        "?token=<value>" on the request path or as "Authorization: Bearer <value>". */
    std::string requiredToken;
};

struct HandshakeResult
{
    bool accepted = false;
    std::string response;     ///< Bytes to send back (101 Switching Protocols, or an HTTP error).
    std::string rejectReason; ///< Human-readable, for logs.
};

HandshakeResult evaluateHandshake (const HttpRequest& request, const HandshakePolicy& policy);

/** The access rules alone (Origin allow-list and token), for plain HTTP
    requests such as MCP's POST /mcp. accepted=false carries a 401/403 response. */
HandshakeResult checkAccess (const HttpRequest& request, const HandshakePolicy& policy);

/** The path part of a request target ("/mcp?x=1" -> "/mcp"). */
std::string requestPath (const HttpRequest& request);

/** A complete HTTP/1.1 response (Connection: close). */
std::string httpResponse (int status, std::string_view statusText, std::string_view contentType, std::string_view body,
                          std::string_view extraHeaders = {});

// ---------------------------------------------------------------------------
// Framing
// ---------------------------------------------------------------------------

/** Encodes one unmasked, unfragmented server-to-client frame. */
std::string encodeFrame (Opcode opcode, std::string_view payload);

/** Encodes a close frame carrying a status code and a short reason. */
std::string encodeCloseFrame (uint16_t code, std::string_view reason);

bool isValidUtf8 (std::string_view text) noexcept;

struct Event
{
    enum class Type
    {
        text,   ///< A complete (possibly reassembled) text message.
        ping,   ///< Reply with a pong carrying the same payload.
        pong,
        close,  ///< Peer closed; echo a close frame and drop the connection.
        error,  ///< Protocol violation; send a close frame with closeCode, then drop.
    };

    Type type = Type::text;
    std::string payload;
    uint16_t code = 0;
};

/** Incremental decoder for client-to-server traffic: feed raw socket bytes in,
    take complete events out. Enforces the client-side rules of RFC 6455
    (frames must be masked, control frames unfragmented and <= 125 bytes, no
    reserved bits, valid UTF-8 text) and a maximum message size. Binary messages
    are rejected because the command protocol is text-only. */
class Reader
{
public:
    explicit Reader (size_t maxMessageBytes = 64 * 1024);

    void feed (const void* data, size_t numBytes);

    /** Next decoded event, if any. After an error event the reader is dead. */
    std::optional<Event> next();

    bool hasFailed() const noexcept { return failed; }

private:
    bool decodeOneFrame();
    void fail (uint16_t code, std::string reason);

    size_t maxMessageBytes;
    std::vector<uint8_t> buffer;
    size_t readPosition = 0;

    bool assemblingMessage = false;
    std::string fragments;

    std::deque<Event> events;
    bool failed = false;
};

} // namespace meridian::remote::ws
