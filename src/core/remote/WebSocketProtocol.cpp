#include "WebSocketProtocol.h"

#include "Base64.h"
#include "Sha1.h"

#include <algorithm>
#include <cctype>

namespace meridian::remote::ws
{
namespace
{
std::string toLower (std::string_view s)
{
    std::string out (s);
    std::transform (out.begin(), out.end(), out.begin(),
                    [] (unsigned char c) { return static_cast<char> (std::tolower (c)); });
    return out;
}

std::string_view trim (std::string_view s)
{
    while (! s.empty() && (s.front() == ' ' || s.front() == '\t'))
        s.remove_prefix (1);
    while (! s.empty() && (s.back() == ' ' || s.back() == '\t'))
        s.remove_suffix (1);
    return s;
}

/** True if a comma-separated header value contains `token` (case-insensitive). */
bool headerHasToken (const std::string& headerValue, std::string_view token)
{
    const auto lowerValue = toLower (headerValue);
    const auto lowerToken = toLower (token);
    std::string_view rest (lowerValue);

    while (! rest.empty())
    {
        const auto comma = rest.find (',');
        if (trim (rest.substr (0, comma)) == lowerToken)
            return true;
        if (comma == std::string_view::npos)
            break;
        rest.remove_prefix (comma + 1);
    }
    return false;
}

int hexValue (char c)
{
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

std::string percentDecode (std::string_view s)
{
    std::string out;
    out.reserve (s.size());
    for (size_t i = 0; i < s.size(); ++i)
    {
        if (s[i] == '%' && i + 2 < s.size() && hexValue (s[i + 1]) >= 0 && hexValue (s[i + 2]) >= 0)
        {
            out += static_cast<char> (hexValue (s[i + 1]) * 16 + hexValue (s[i + 2]));
            i += 2;
        }
        else
        {
            out += s[i] == '+' ? ' ' : s[i];
        }
    }
    return out;
}

std::optional<std::string> queryParameter (std::string_view target, std::string_view name)
{
    const auto q = target.find ('?');
    if (q == std::string_view::npos)
        return std::nullopt;

    std::string_view query = target.substr (q + 1);
    while (! query.empty())
    {
        const auto amp = query.find ('&');
        const auto pair = query.substr (0, amp);
        const auto eq = pair.find ('=');
        if (pair.substr (0, eq) == name)
            return percentDecode (eq == std::string_view::npos ? std::string_view {} : pair.substr (eq + 1));
        if (amp == std::string_view::npos)
            break;
        query.remove_prefix (amp + 1);
    }
    return std::nullopt;
}

/** Comparison whose duration does not depend on where the strings differ. */
bool constantTimeEquals (std::string_view a, std::string_view b)
{
    unsigned char diff = a.size() == b.size() ? 0 : 1;
    const size_t n = std::max (a.size(), b.size());
    for (size_t i = 0; i < n; ++i)
    {
        const auto ca = i < a.size() ? static_cast<unsigned char> (a[i]) : 0;
        const auto cb = i < b.size() ? static_cast<unsigned char> (b[i]) : 0;
        diff = static_cast<unsigned char> (diff | (ca ^ cb));
    }
    return diff == 0;
}

std::string httpError (int status, std::string_view statusText, std::string_view body, std::string_view extraHeaders = {})
{
    std::string r = "HTTP/1.1 " + std::to_string (status) + " " + std::string (statusText) + "\r\n";
    r += "Content-Type: text/plain; charset=utf-8\r\n";
    r += "Content-Length: " + std::to_string (body.size()) + "\r\n";
    r += extraHeaders;
    r += "Connection: close\r\n\r\n";
    r += body;
    return r;
}

HandshakeResult reject (int status, std::string_view statusText, std::string reason, std::string_view extraHeaders = {})
{
    HandshakeResult result;
    result.accepted = false;
    result.response = httpError (status, statusText, reason + "\n", extraHeaders);
    result.rejectReason = std::move (reason);
    return result;
}
} // namespace

// ---------------------------------------------------------------------------

std::string HttpRequest::header (const std::string& lowerCaseName) const
{
    const auto it = headers.find (lowerCaseName);
    return it != headers.end() ? it->second : std::string {};
}

std::optional<HttpRequest> parseHttpRequest (std::string_view head)
{
    HttpRequest request;

    const auto firstLineEnd = head.find ("\r\n");
    if (firstLineEnd == std::string_view::npos)
        return std::nullopt;

    const auto requestLine = head.substr (0, firstLineEnd);
    const auto sp1 = requestLine.find (' ');
    const auto sp2 = requestLine.rfind (' ');
    if (sp1 == std::string_view::npos || sp2 == sp1)
        return std::nullopt;

    request.method = std::string (requestLine.substr (0, sp1));
    request.target = std::string (requestLine.substr (sp1 + 1, sp2 - sp1 - 1));
    request.version = std::string (requestLine.substr (sp2 + 1));

    std::string_view rest = head.substr (firstLineEnd + 2);
    while (! rest.empty())
    {
        const auto lineEnd = rest.find ("\r\n");
        const auto line = rest.substr (0, lineEnd);
        if (line.empty())
            break; // blank line: end of head

        const auto colon = line.find (':');
        if (colon == std::string_view::npos || colon == 0)
            return std::nullopt;

        auto name = toLower (trim (line.substr (0, colon)));
        auto value = std::string (trim (line.substr (colon + 1)));

        auto& slot = request.headers[name];
        slot = slot.empty() ? value : slot + ", " + value; // repeated headers fold into a list

        if (lineEnd == std::string_view::npos)
            break;
        rest.remove_prefix (lineEnd + 2);
    }
    return request;
}

std::string computeAcceptKey (std::string_view clientKey)
{
    static constexpr std::string_view guid = "258EAFA5-E914-47DA-95CA-C5AB0DC85B11";
    std::string input (clientKey);
    input += guid;
    const auto digest = sha1 (input.data(), input.size());
    return base64Encode (digest.data(), digest.size());
}

HandshakeResult evaluateHandshake (const HttpRequest& request, const HandshakePolicy& policy)
{
    if (request.method != "GET" || request.version != "HTTP/1.1")
        return reject (400, "Bad Request", "WebSocket handshakes must be HTTP/1.1 GET requests");

    if (request.target.empty() || request.target.front() != '/')
        return reject (400, "Bad Request", "Invalid request target");

    if (! headerHasToken (request.header ("upgrade"), "websocket")
        || ! headerHasToken (request.header ("connection"), "upgrade"))
        return reject (426, "Upgrade Required", "This port only speaks WebSocket", "Upgrade: websocket\r\n");

    if (request.header ("sec-websocket-version") != "13")
        return reject (426, "Upgrade Required", "Only WebSocket version 13 is supported", "Sec-WebSocket-Version: 13\r\n");

    const auto key = request.header ("sec-websocket-key");
    if (key.size() != 24 || key.substr (22) != "==") // base64 of exactly 16 bytes
        return reject (400, "Bad Request", "Missing or malformed Sec-WebSocket-Key");

    if (auto access = checkAccess (request, policy); ! access.accepted)
        return access;

    HandshakeResult result;
    result.accepted = true;
    result.response = "HTTP/1.1 101 Switching Protocols\r\n"
                      "Upgrade: websocket\r\n"
                      "Connection: Upgrade\r\n"
                      "Sec-WebSocket-Accept: " + computeAcceptKey (key) + "\r\n\r\n";
    return result;
}

std::string requestPath (const HttpRequest& request)
{
    return request.target.substr (0, request.target.find ('?'));
}

std::string httpResponse (int status, std::string_view statusText, std::string_view contentType, std::string_view body,
                          std::string_view extraHeaders)
{
    std::string r = "HTTP/1.1 " + std::to_string (status) + " " + std::string (statusText) + "\r\n";
    if (! contentType.empty())
        r += "Content-Type: " + std::string (contentType) + "\r\n";
    r += "Content-Length: " + std::to_string (body.size()) + "\r\n";
    r += extraHeaders;
    r += "Connection: close\r\n\r\n";
    r += body;
    return r;
}

HandshakeResult checkAccess (const HttpRequest& request, const HandshakePolicy& policy)
{
    const auto origin = request.header ("origin");
    if (! origin.empty())
    {
        const auto lowerOrigin = toLower (origin);
        const bool allowed = std::any_of (policy.allowedOrigins.begin(), policy.allowedOrigins.end(),
                                          [&] (const std::string& o) { return toLower (o) == lowerOrigin; });
        if (! allowed)
            return reject (403, "Forbidden", "Origin '" + origin + "' is not allowed to control this application");
    }

    if (! policy.requiredToken.empty())
    {
        std::string presented;
        if (auto fromQuery = queryParameter (request.target, "token"))
        {
            presented = *fromQuery;
        }
        else
        {
            const auto auth = request.header ("authorization");
            constexpr std::string_view bearer = "Bearer ";
            if (auth.size() > bearer.size() && toLower (auth.substr (0, bearer.size())) == "bearer ")
                presented = std::string (trim (std::string_view (auth).substr (bearer.size())));
        }

        if (! constantTimeEquals (presented, policy.requiredToken))
            return reject (401, "Unauthorized", "A valid access token is required");
    }

    HandshakeResult result;
    result.accepted = true;
    return result;
}

// ---------------------------------------------------------------------------

std::string encodeFrame (Opcode opcode, std::string_view payload)
{
    std::string frame;
    frame.reserve (payload.size() + 10);
    frame += static_cast<char> (0x80 | static_cast<uint8_t> (opcode)); // FIN + opcode

    const uint64_t length = payload.size();
    if (length < 126)
    {
        frame += static_cast<char> (length);
    }
    else if (length <= 0xFFFF)
    {
        frame += static_cast<char> (126);
        frame += static_cast<char> ((length >> 8) & 0xFF);
        frame += static_cast<char> (length & 0xFF);
    }
    else
    {
        frame += static_cast<char> (127);
        for (int shift = 56; shift >= 0; shift -= 8)
            frame += static_cast<char> ((length >> shift) & 0xFF);
    }

    frame.append (payload.data(), payload.size());
    return frame;
}

std::string encodeCloseFrame (uint16_t code, std::string_view reason)
{
    std::string payload;
    payload += static_cast<char> (code >> 8);
    payload += static_cast<char> (code & 0xFF);
    payload.append (reason.substr (0, 123)); // control frame payloads are capped at 125 bytes
    return encodeFrame (Opcode::close, payload);
}

bool isValidUtf8 (std::string_view text) noexcept
{
    size_t i = 0;
    const size_t n = text.size();
    while (i < n)
    {
        const auto c = static_cast<unsigned char> (text[i]);
        if (c < 0x80)
        {
            ++i;
            continue;
        }

        int extra = 0;
        uint32_t codepoint = 0;
        if ((c & 0xE0) == 0xC0)      { extra = 1; codepoint = c & 0x1F; }
        else if ((c & 0xF0) == 0xE0) { extra = 2; codepoint = c & 0x0F; }
        else if ((c & 0xF8) == 0xF0) { extra = 3; codepoint = c & 0x07; }
        else return false;

        if (i + static_cast<size_t> (extra) >= n) // sequence truncated by end of text
            return false;

        for (int k = 1; k <= extra; ++k)
        {
            const auto cc = static_cast<unsigned char> (text[i + static_cast<size_t> (k)]);
            if ((cc & 0xC0) != 0x80)
                return false;
            codepoint = (codepoint << 6) | (cc & 0x3F);
        }

        // Reject overlong encodings, UTF-16 surrogates and values past U+10FFFF.
        static constexpr uint32_t minimumForLength[] = { 0, 0x80, 0x800, 0x10000 };
        if (codepoint < minimumForLength[extra] || codepoint > 0x10FFFF
            || (codepoint >= 0xD800 && codepoint <= 0xDFFF))
            return false;

        i += static_cast<size_t> (extra) + 1;
    }
    return true;
}

// ---------------------------------------------------------------------------

Reader::Reader (size_t maxBytes) : maxMessageBytes (maxBytes) {}

void Reader::feed (const void* data, size_t numBytes)
{
    if (failed || numBytes == 0)
        return;

    // Compact consumed bytes before growing, so the buffer stays bounded.
    if (readPosition > 0 && (readPosition == buffer.size() || readPosition > 65536))
    {
        buffer.erase (buffer.begin(), buffer.begin() + static_cast<std::ptrdiff_t> (readPosition));
        readPosition = 0;
    }

    const auto* bytes = static_cast<const uint8_t*> (data);
    buffer.insert (buffer.end(), bytes, bytes + numBytes);
}

std::optional<Event> Reader::next()
{
    while (events.empty() && ! failed && decodeOneFrame())
    {
    }

    if (events.empty())
        return std::nullopt;

    auto event = std::move (events.front());
    events.pop_front();
    return event;
}

void Reader::fail (uint16_t code, std::string reason)
{
    failed = true;
    Event e;
    e.type = Event::Type::error;
    e.code = code;
    e.payload = std::move (reason);
    events.push_back (std::move (e));
}

bool Reader::decodeOneFrame()
{
    const size_t available = buffer.size() - readPosition;
    if (available < 2)
        return false;

    const uint8_t* p = buffer.data() + readPosition;
    const bool fin = (p[0] & 0x80) != 0;
    const uint8_t reserved = p[0] & 0x70;
    const uint8_t op = p[0] & 0x0F;
    const bool masked = (p[1] & 0x80) != 0;
    uint64_t length = p[1] & 0x7F;
    size_t headerSize = 2;

    if (reserved != 0)
    {
        fail (closeCode::protocolError, "Reserved bits must be zero (no extensions were negotiated)");
        return false;
    }

    if (length == 126)
    {
        if (available < 4)
            return false;
        length = (uint64_t (p[2]) << 8) | p[3];
        headerSize = 4;
    }
    else if (length == 127)
    {
        if (available < 10)
            return false;
        length = 0;
        for (int i = 0; i < 8; ++i)
            length = (length << 8) | p[2 + i];
        headerSize = 10;
        if ((length >> 63) != 0)
        {
            fail (closeCode::protocolError, "Invalid 64-bit payload length");
            return false;
        }
    }

    if (! masked)
    {
        fail (closeCode::protocolError, "Client-to-server frames must be masked");
        return false;
    }

    const bool isControl = (op & 0x08) != 0;
    if (isControl && (! fin || length > 125))
    {
        fail (closeCode::protocolError, "Control frames must be unfragmented and at most 125 bytes");
        return false;
    }

    if (length > maxMessageBytes)
    {
        fail (closeCode::messageTooBig, "Message exceeds the size limit");
        return false;
    }

    const size_t frameSize = headerSize + 4 + static_cast<size_t> (length);
    if (available < frameSize)
        return false;

    const uint8_t* mask = p + headerSize;
    const uint8_t* maskedPayload = mask + 4;
    std::string payload (static_cast<size_t> (length), '\0');
    for (size_t i = 0; i < payload.size(); ++i)
        payload[i] = static_cast<char> (maskedPayload[i] ^ mask[i & 3]);

    readPosition += frameSize;

    switch (static_cast<Opcode> (op))
    {
        case Opcode::continuation:
            if (! assemblingMessage)
            {
                fail (closeCode::protocolError, "Continuation frame without a message in progress");
                return false;
            }
            fragments += payload;
            if (fragments.size() > maxMessageBytes)
            {
                fail (closeCode::messageTooBig, "Message exceeds the size limit");
                return false;
            }
            if (fin)
            {
                assemblingMessage = false;
                if (! isValidUtf8 (fragments))
                {
                    fail (closeCode::invalidPayload, "Text message is not valid UTF-8");
                    return false;
                }
                events.push_back ({ Event::Type::text, std::move (fragments), 0 });
                fragments.clear();
            }
            return true;

        case Opcode::text:
            if (assemblingMessage)
            {
                fail (closeCode::protocolError, "New message started before the previous one finished");
                return false;
            }
            if (fin)
            {
                if (! isValidUtf8 (payload))
                {
                    fail (closeCode::invalidPayload, "Text message is not valid UTF-8");
                    return false;
                }
                events.push_back ({ Event::Type::text, std::move (payload), 0 });
            }
            else
            {
                assemblingMessage = true;
                fragments = std::move (payload);
            }
            return true;

        case Opcode::binary:
            fail (closeCode::unsupportedData, "Binary messages are not supported; send text commands");
            return false;

        case Opcode::close:
        {
            if (payload.size() == 1)
            {
                fail (closeCode::protocolError, "Malformed close frame");
                return false;
            }
            Event e;
            e.type = Event::Type::close;
            e.code = closeCode::normal;
            if (payload.size() >= 2)
            {
                e.code = static_cast<uint16_t> ((static_cast<uint8_t> (payload[0]) << 8) | static_cast<uint8_t> (payload[1]));
                e.payload = payload.substr (2);
            }
            events.push_back (std::move (e));
            return true;
        }

        case Opcode::ping:
            events.push_back ({ Event::Type::ping, std::move (payload), 0 });
            return true;

        case Opcode::pong:
            events.push_back ({ Event::Type::pong, std::move (payload), 0 });
            return true;

        default:
            fail (closeCode::protocolError, "Unknown opcode");
            return false;
    }
}

} // namespace meridian::remote::ws
