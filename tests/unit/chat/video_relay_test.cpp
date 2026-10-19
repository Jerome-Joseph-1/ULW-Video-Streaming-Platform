#include "core/ports/video_events.hpp"
#include "core/util/json.hpp"
#include "net/reactor_factory.hpp"

#include "support/fake_clock.hpp"
#include "support/fake_random.hpp"
#include "video_relay.hpp"

#include <cstddef>
#include <format>
#include <gtest/gtest.h>
#include <memory>
#include <optional>
#include <string>
#include <utility>
#include <vector>

// ADR-0120: videos that became ready or failed, taken from the catalog and sent on the hook.
namespace {

using core::ports::NotifyCallback;
using core::ports::NotifyError;
using core::ports::VideoEvent;

constexpr std::string_view kVideo = "01890a5d-ac96-774b-bcce-b302099a8057";

class CollectingSink final : public chat::IEventSink {
public:
    void offer(std::string event) noexcept override { events.push_back(std::move(event)); }
    std::vector<std::string> events;
};

// Answers each take when told to.
class FakeVideoEvents final : public core::ports::IVideoEvents {
public:
    void take_video_events(std::size_t max, NotifyCallback<std::vector<VideoEvent>> done) override {
        asked.push_back(max);
        waiting.push_back(std::move(done));
    }
    void answer(core::ports::NotifyResult<std::vector<VideoEvent>> result) {
        ASSERT_FALSE(waiting.empty());
        auto done = std::move(waiting.front());
        waiting.erase(waiting.begin());
        done(std::move(result));
    }
    std::vector<std::size_t> asked;
    std::vector<NotifyCallback<std::vector<VideoEvent>>> waiting;
};

VideoEvent ready(int n = 0) {
    return VideoEvent{
        .video = *core::VideoId::parse(
            n == 0 ? std::string(kVideo) : std::format("01890a5d-ac96-774b-bcce-b302099a{:04}", n)),
        .owner = *core::UserId::parse("auth0|alice"),
        .ready = true,
        .version = 3,
        .duration = core::Millis{6'000},
        .error_reason = std::nullopt,
        .at_ms = 1'767'225'600'250};
}

class VideoRelayTest : public ::testing::Test {
protected:
    void SetUp() override {
        auto r = net::make_reactor(net::ReactorKind::Epoll, clock_, 64);
        ASSERT_TRUE(r);
        reactor_ = std::move(*r);
        relay_ = std::make_unique<chat::VideoRelay>(*reactor_, store_, sink_, clock_, random_);
    }
    void TearDown() override {
        relay_.reset();
        reactor_.reset();
    }
    void wait(core::Millis d) {
        clock_.advance(d);
        for (int i = 0; i < 4; ++i) {
            reactor_->run_once(core::Millis{0});
        }
    }

    ulw::test::FakeClock clock_;
    ulw::test::FakeRandom random_;
    std::unique_ptr<net::IReactor> reactor_;
    FakeVideoEvents store_;
    CollectingSink sink_;
    std::unique_ptr<chat::VideoRelay> relay_;
};

TEST_F(VideoRelayTest, EachChangeIsOneEventWithWhatTheBackendNeeds) {
    relay_->start();
    ASSERT_EQ(store_.asked, std::vector<std::size_t>{core::ports::kMaxVideoEvents});
    VideoEvent failed = ready(1);
    failed.ready = false;
    failed.duration.reset();
    failed.error_reason = "no \"video\" stream";
    store_.answer(std::vector<VideoEvent>{ready(), failed});
    ASSERT_EQ(sink_.events.size(), 2U);
    EXPECT_EQ(relay_->counters().events, 2U);

    const auto first = core::json::parse(sink_.events[0]);
    ASSERT_TRUE(first) << sink_.events[0];
    EXPECT_EQ(first->find("type")->as_string(), "video_ready");
    EXPECT_EQ(first->find("video")->as_string(), kVideo);
    EXPECT_EQ(first->find("owner")->as_string(), "auth0|alice");
    // When the video changed, in seconds, not when the relay took it.
    EXPECT_NE(sink_.events[0].find(R"("at":1767225600,)"), std::string::npos) << sink_.events[0];
    EXPECT_NE(sink_.events[0].find(R"("version":3,"duration_ms":6000})"), std::string::npos)
        << sink_.events[0];
    EXPECT_EQ(first->find("id")->as_string()->size(), 36U);

    const auto second = core::json::parse(sink_.events[1]);
    ASSERT_TRUE(second) << sink_.events[1];
    EXPECT_EQ(second->find("type")->as_string(), "video_failed");
    EXPECT_EQ(second->find("error_reason")->as_string(), "no \"video\" stream");
    EXPECT_EQ(second->find("duration_ms"), nullptr);
    EXPECT_NE(first->find("id")->as_string(), second->find("id")->as_string());
}

TEST_F(VideoRelayTest, TheCatalogIsAskedAgainEveryPollAndAtOnceAfterAFullBatch) {
    relay_->start();
    store_.answer(std::vector<VideoEvent>{});
    EXPECT_TRUE(store_.waiting.empty());
    wait(core::Millis{1'999});
    EXPECT_TRUE(store_.waiting.empty());
    wait(core::Millis{1});
    ASSERT_EQ(store_.waiting.size(), 1U);

    std::vector<VideoEvent> full;
    for (std::size_t i = 1; i <= core::ports::kMaxVideoEvents; ++i) {
        full.push_back(ready(static_cast<int>(i)));
    }
    store_.answer(std::move(full));
    // More wait: asked again without waiting.
    EXPECT_EQ(store_.waiting.size(), 1U);
    EXPECT_EQ(sink_.events.size(), core::ports::kMaxVideoEvents);
}

TEST_F(VideoRelayTest, AFailedTakeIsTriedAgainAtTheNextPoll) {
    relay_->start();
    store_.answer(std::unexpected(NotifyError::Unavailable));
    EXPECT_EQ(relay_->counters().take_failures, 1U);
    EXPECT_TRUE(sink_.events.empty());
    wait(core::Millis{2'000});
    ASSERT_EQ(store_.waiting.size(), 1U);
    store_.answer(std::vector<VideoEvent>{ready()});
    EXPECT_EQ(sink_.events.size(), 1U);
}

TEST_F(VideoRelayTest, AfterStopNothingIsTakenOrSent) {
    relay_->start();
    relay_->stop();
    store_.answer(std::vector<VideoEvent>{ready()});
    EXPECT_TRUE(sink_.events.empty());
    wait(core::Millis{10'000});
    EXPECT_TRUE(store_.waiting.empty());
    // An answer after the relay is gone goes nowhere.
    relay_->start();
    EXPECT_TRUE(store_.waiting.empty());
}

} // namespace
