#pragma once

#include <remote/WebSocketProtocol.h>

#include <juce_events/juce_events.h>

#include <atomic>
#include <functional>
#include <memory>
#include <string>
#include <vector>

namespace meridian
{

/** The AI remote-control port: a WebSocket server on 127.0.0.1.

    One accept thread plus one thread per client (at most maxClients). Socket
    I/O never touches the message thread; each received text message is
    handed to the message thread with callAsync, executed there by the
    handler (the only place application state may change), and the reply is
    sent back on the client's own thread. The UI therefore never blocks on the
    network, and the network never races the UI.

    Security: binds to the loopback interface only; refuses browser origins
    not explicitly allowed (any website could otherwise script a localhost
    socket); optional access token. See ws::HandshakePolicy. */
class RemoteControlServer final : private juce::Thread
{
public:
    /** Runs on the message thread; returns the JSON reply. */
    using MessageHandler = std::function<std::string (const std::string&)>;

    static constexpr int defaultPort = 47800;
    static constexpr int maxClients = 8;

    explicit RemoteControlServer (MessageHandler handler);
    ~RemoteControlServer() override;

    bool start (int port, remote::ws::HandshakePolicy policy, juce::String& error);
    void stop();

    bool isRunning() const noexcept   { return running.load(); }
    int getPort() const noexcept      { return port; }
    int getNumClients() const noexcept { return numClients.load(); }

private:
    class Connection;

    void run() override; // accept loop
    void reapFinishedConnections();

    MessageHandler handler;
    remote::ws::HandshakePolicy policy;
    juce::StreamingSocket listener;
    int port = defaultPort;

    std::shared_ptr<std::atomic<bool>> alive; // guards callAsync work queued after stop()
    std::vector<std::unique_ptr<Connection>> connections;
    std::atomic<bool> running { false };
    std::atomic<int> numClients { 0 };

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (RemoteControlServer)
};

} // namespace meridian
