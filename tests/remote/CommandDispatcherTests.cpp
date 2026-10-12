#include <remote/CommandDispatcher.h>

#include <catch2/catch_test_macros.hpp>

#include <string>
#include <vector>

using namespace meridian::remote;

namespace
{
class FakeActions final : public RemoteActions
{
public:
    std::vector<std::string> calls;

    ActionResult play() override          { calls.push_back ("play"); return ActionResult::success ("Playing"); }
    ActionResult stop() override          { calls.push_back ("stop"); return ActionResult::success ("Stopped"); }
    ActionResult record() override        { calls.push_back ("record"); return ActionResult::failure ("No tracks are record-armed"); }
    ActionResult toggleWindow() override  { calls.push_back ("toggle"); return ActionResult::success ("Mix window"); }

    ActionResult toggleTrackMute (int track) override
    {
        calls.push_back ("mute " + std::to_string (track));
        return track <= 2 ? ActionResult::success ("Track muted") : ActionResult::failure ("No track " + std::to_string (track));
    }

    ActionResult setTrackVolume (int track, double db) override
    {
        calls.push_back ("volume " + std::to_string (track) + " " + std::to_string (db));
        return ActionResult::success ("Ramping");
    }

    std::string statusJson() override { return R"({"transport":"stopped"})"; }
};

bool contains (const std::string& haystack, const std::string& needle) { return haystack.find (needle) != std::string::npos; }
} // namespace

TEST_CASE ("Dispatcher routes each command to exactly one action", "[dispatcher]")
{
    FakeActions actions;

    REQUIRE (contains (handleRemoteMessage ("PLAY", actions), R"("ok":true)"));
    REQUIRE (contains (handleRemoteMessage ("stop", actions), R"("command":"STOP")"));
    handleRemoteMessage ("TOGGLE_WINDOW", actions);
    handleRemoteMessage ("MUTE_TRACK_2", actions);
    handleRemoteMessage ("SET_VOLUME_TRACK_1_-6dB", actions);

    REQUIRE (actions.calls == std::vector<std::string> { "play", "stop", "toggle", "mute 2", "volume 1 -6.000000" });
}

TEST_CASE ("Action failures are reported, not swallowed", "[dispatcher]")
{
    FakeActions actions;
    const auto reply = handleRemoteMessage ("RECORD", actions);
    REQUIRE (contains (reply, R"("ok":false)"));
    REQUIRE (contains (reply, R"("error":"No tracks are record-armed")"));

    REQUIRE (contains (handleRemoteMessage ("MUTE_TRACK_7", actions), R"("error":"No track 7")"));
}

TEST_CASE ("Parse errors never reach the application", "[dispatcher]")
{
    FakeActions actions;
    const auto reply = handleRemoteMessage ("MUTE_TRACK_X", actions);
    REQUIRE (actions.calls.empty());
    REQUIRE (contains (reply, R"("ok":false)"));
    REQUIRE (contains (reply, R"("input":"MUTE_TRACK_X")"));
}

TEST_CASE ("Replies are valid JSON even for hostile input", "[dispatcher][security]")
{
    FakeActions actions;
    const auto reply = handleRemoteMessage ("\"quoted\" \\ back\nslash", actions);
    REQUIRE (contains (reply, R"(\"quoted\" \\ back\nslash)"));
}

TEST_CASE ("STATUS embeds the application's JSON", "[dispatcher]")
{
    FakeActions actions;
    REQUIRE (handleRemoteMessage ("STATUS", actions)
             == R"({"ok":true,"command":"STATUS","status":{"transport":"stopped"}})");
}
