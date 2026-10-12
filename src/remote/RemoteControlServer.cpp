#include "RemoteControlServer.h"

#include <remote/JsonWriter.h>

#include <algorithm>

namespace meridian
{
namespace ws = remote::ws;

namespace
{
constexpr size_t maxRequestHeadBytes = 8192;
constexpr int handshakeTimeoutMs = 5000;
constexpr int pollIntervalMs = 100;

std::string errorJson (std::string_view message)
{
    return remote::JsonObject().addBool ("ok", false).addString ("error", message).toString();
}
} // namespace

//==============================================================================
class RemoteControlServer::Connection final : public juce::Thread
{
public:
    Connection (std::unique_ptr<juce::StreamingSocket> s, RemoteControlServer& o)
        : juce::Thread ("Remote Control Client"), socket (std::move (s)), owner (o)
    {
    }

    ~Connection() override { closeAndStop(); }

    void closeAndStop()
    {
        signalThreadShouldExit();
        socket->close();
        stopThread (2000);
    }

    bool isFinished() const noexcept { return finished.load(); }

private:
    void run() override
    {
        ++owner.numClients;

        std::string leftover;
        ws::HttpRequest request;
        if (readRequest (request, leftover))
        {
            const auto path = ws::requestPath (request);
            if (path == "/mcp" && request.method == "POST")
                serveMcp (request, leftover);
            else if (path == "/mcp")
                sendAll (ws::httpResponse (405, "Method Not Allowed", "text/plain", "Use POST for MCP requests.\n", "Allow: POST\r\n"));
            else if (performHandshake (request))
                serve (leftover);
        }

        socket->close();
        --owner.numClients;
        finished.store (true);
    }

    bool sendAll (const std::string& data)
    {
        size_t sent = 0;
        while (sent < data.size())
        {
            const int n = socket->write (data.data() + sent, (int) (data.size() - sent));
            if (n <= 0)
                return false;
            sent += (size_t) n;
        }
        return true;
    }

    /** Waits for data; returns bytes read, 0 on timeout, -1 on close/error. */
    int receive (char* buffer, int size)
    {
        const int ready = socket->waitUntilReady (true, pollIntervalMs);
        if (ready < 0)
            return -1;
        if (ready == 0)
            return 0;
        const int n = socket->read (buffer, size, false);
        return n > 0 ? n : -1;
    }

    /** Reads and parses the HTTP request head. Bytes after it go to `leftover`. */
    bool readRequest (ws::HttpRequest& request, std::string& leftover)
    {
        std::string head;
        const auto deadline = juce::Time::getMillisecondCounter() + (juce::uint32) handshakeTimeoutMs;
        char buffer[1024];

        while (! threadShouldExit())
        {
            if (juce::Time::getMillisecondCounter() > deadline)
                return false;

            const int n = receive (buffer, (int) sizeof (buffer));
            if (n < 0)
                return false;
            head.append (buffer, (size_t) n);

            if (const auto end = head.find ("\r\n\r\n"); end != std::string::npos)
            {
                leftover = head.substr (end + 4);
                head.resize (end + 4);
                break;
            }
            if (head.size() > maxRequestHeadBytes)
                return false;
        }

        auto parsed = ws::parseHttpRequest (head);
        if (! parsed)
        {
            sendAll (ws::httpResponse (400, "Bad Request", "text/plain", "Malformed HTTP request.\n"));
            return false;
        }
        request = std::move (*parsed);
        return true;
    }

    bool performHandshake (const ws::HttpRequest& request)
    {
        const auto result = ws::evaluateHandshake (request, owner.policy);
        sendAll (result.response);
        if (! result.accepted)
        {
            DBG ("Remote control: refused connection: " << juce::String (result.rejectReason)); // empty in Release
        }
        return result.accepted;
    }

    /** One MCP JSON-RPC exchange over HTTP, then the connection closes. */
    void serveMcp (const ws::HttpRequest& request, std::string body)
    {
        if (const auto access = ws::checkAccess (request, owner.policy); ! access.accepted)
        {
            sendAll (access.response);
            return;
        }

        const auto lengthHeader = request.header ("content-length");
        const auto length = (size_t) std::strtoull (lengthHeader.c_str(), nullptr, 10);
        if (lengthHeader.empty() || length > maxHttpBodyBytes)
        {
            sendAll (ws::httpResponse (lengthHeader.empty() ? 411 : 413, lengthHeader.empty() ? "Length Required" : "Payload Too Large",
                                       "text/plain", "Send a JSON-RPC body with Content-Length (max 1 MB).\n"));
            return;
        }

        const auto deadline = juce::Time::getMillisecondCounter() + (juce::uint32) handshakeTimeoutMs;
        char buffer[4096];
        while (body.size() < length && ! threadShouldExit())
        {
            if (juce::Time::getMillisecondCounter() > deadline)
                return;
            const int n = receive (buffer, (int) sizeof (buffer));
            if (n < 0)
                return;
            body.append (buffer, (size_t) n);
        }
        body.resize (length);

        auto statusHolder = std::make_shared<int> (200); // outlives this frame if shutdown abandons the call
        const auto reply = runOnMessageThread ([handler = owner.mcpHandler, body, statusHolder]
        {
            return handler (body, *statusHolder);
        });
        const int status = *statusHolder;

        if (status == 202)
            sendAll (ws::httpResponse (202, "Accepted", {}, {}));
        else
            sendAll (ws::httpResponse (status, "OK", "application/json", reply));
    }

    void serve (const std::string& leftover)
    {
        ws::Reader reader (64 * 1024);
        reader.feed (leftover.data(), leftover.size());
        char buffer[4096];

        while (! threadShouldExit())
        {
            while (auto event = reader.next())
            {
                switch (event->type)
                {
                    case ws::Event::Type::text:
                        if (! sendAll (ws::encodeFrame (ws::Opcode::text, dispatch (event->payload))))
                            return;
                        break;

                    case ws::Event::Type::ping:
                        if (! sendAll (ws::encodeFrame (ws::Opcode::pong, event->payload)))
                            return;
                        break;

                    case ws::Event::Type::pong:
                        break;

                    case ws::Event::Type::close:
                        sendAll (ws::encodeCloseFrame (ws::closeCode::normal, {}));
                        return;

                    case ws::Event::Type::error:
                        sendAll (ws::encodeCloseFrame (event->code, event->payload));
                        return;
                }
            }

            const int n = receive (buffer, (int) sizeof (buffer));
            if (n < 0)
                return;
            reader.feed (buffer, (size_t) n);
        }

        sendAll (ws::encodeCloseFrame (ws::closeCode::goingAway, "Server shutting down"));
    }

    std::string dispatch (const std::string& text)
    {
        return runOnMessageThread ([handler = owner.handler, text] { return handler (text); });
    }

    /** Runs `work` on the message thread and waits for its result, without
        ever blocking shutdown. `work` must own everything it touches (capture
        by value or shared_ptr): on shutdown this may return before it runs. */
    std::string runOnMessageThread (std::function<std::string()> work)
    {
        struct Pending
        {
            juce::WaitableEvent done;
            std::string reply;
            std::atomic<bool> abandoned { false };
        };

        auto pending = std::make_shared<Pending>();
        const bool posted = juce::MessageManager::callAsync (
            [pending, alive = owner.alive, work]
            {
                if (alive->load() && ! pending->abandoned.load())
                    pending->reply = work();
                else
                    pending->reply = errorJson ("The application is shutting down.");
                pending->done.signal();
            });

        if (! posted)
            return errorJson ("The application is shutting down.");

        while (! pending->done.wait (50))
        {
            if (threadShouldExit())
            {
                pending->abandoned.store (true); // `work` must not run once we have returned
                return errorJson ("The application is shutting down.");
            }
        }

        return pending->reply;
    }

    std::unique_ptr<juce::StreamingSocket> socket;
    RemoteControlServer& owner;
    std::atomic<bool> finished { false };
};

//==============================================================================
RemoteControlServer::RemoteControlServer (MessageHandler h, McpHandler mcp)
    : juce::Thread ("Remote Control Server"),
      handler (std::move (h)),
      mcpHandler (std::move (mcp)),
      alive (std::make_shared<std::atomic<bool>> (false))
{
}

RemoteControlServer::~RemoteControlServer()
{
    stop();
}

bool RemoteControlServer::start (int newPort, remote::ws::HandshakePolicy newPolicy, juce::String& error)
{
    stop();

    policy = std::move (newPolicy);
    port = newPort;

    // Loopback only: the port is never reachable from the network.
    if (! listener.createListener (port, "127.0.0.1"))
    {
        error = "Could not listen on 127.0.0.1:" + juce::String (port) + ". Another program may be using that port.";
        return false;
    }

    alive = std::make_shared<std::atomic<bool>> (true);
    running.store (true);
    startThread();
    return true;
}

void RemoteControlServer::stop()
{
    alive->store (false);
    running.store (false);

    signalThreadShouldExit();
    listener.close(); // unblocks waitForNextConnection()
    stopThread (3000);

    for (auto& c : connections)
        c->closeAndStop();
    connections.clear();
}

void RemoteControlServer::reapFinishedConnections()
{
    connections.erase (std::remove_if (connections.begin(), connections.end(),
                                       [] (const std::unique_ptr<Connection>& c) { return c->isFinished(); }),
                       connections.end());
}

void RemoteControlServer::run()
{
    while (! threadShouldExit())
    {
        std::unique_ptr<juce::StreamingSocket> client (listener.waitForNextConnection());
        if (threadShouldExit())
            break;
        if (client == nullptr)
        {
            wait (50);
            continue;
        }

        reapFinishedConnections();

        if ((int) connections.size() >= maxClients)
        {
            const std::string busy = "HTTP/1.1 503 Service Unavailable\r\nContent-Length: 0\r\nConnection: close\r\n\r\n";
            client->write (busy.data(), (int) busy.size());
            client->close();
            continue;
        }

        auto connection = std::make_unique<Connection> (std::move (client), *this);
        connection->startThread();
        connections.push_back (std::move (connection));
    }
}

} // namespace meridian
