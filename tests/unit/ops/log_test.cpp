#include "core/util/json.hpp"

#include "ops/log.hpp"
#include "support/fake_clock.hpp"
#include "support/memory_log.hpp"

#include <chrono>
#include <gtest/gtest.h>
#include <string>

namespace {

using ulw::test::MemoryLog;

class LoggerTest : public ::testing::Test {
protected:
    LoggerTest() { clock.advance(std::chrono::milliseconds(3'723'045)); }

    ulw::test::FakeClock clock;
    MemoryLog sink;
    ops::Logger log{sink, clock, "gateway", ops::Level::Info};
};

TEST_F(LoggerTest, ALineIsOneJsonObjectWithTheFixedFieldsFirst) {
    log.info("request", {{"status", 204}, {"route", "append_chunk"}, {"ok", true}});
    ASSERT_EQ(sink.lines().size(), 1U);
    EXPECT_EQ(sink.lines()[0],
              R"({"ts":"2026-01-01T01:02:03.045Z","level":"info","svc":"gateway",)"
              R"("event":"request","status":204,"route":"append_chunk","ok":true})");
}

TEST_F(LoggerTest, NumbersKeepTheirSignAndWidth) {
    log.info("n", {{"neg", -5}, {"big", std::uint64_t{18'446'744'073'709'551'615U}}});
    EXPECT_NE(sink.lines()[0].find(R"("neg":-5,"big":18446744073709551615})"), std::string::npos);
}

TEST_F(LoggerTest, TextThatCouldEndTheStringOrTheLineIsEscaped) {
    log.warn("odd", {{"detail", "a\"b\\c\nd\x01"}});
    const std::string line = sink.lines().at(0);
    EXPECT_EQ(line.find('\n'), std::string::npos);
    const auto doc = core::json::parse(line);
    ASSERT_TRUE(doc) << line;
    EXPECT_EQ(doc->find("detail")->as_string(), "a\"b\\c\nd\x01");
}

TEST_F(LoggerTest, LinesBelowTheThresholdAreNotWritten) {
    log.debug("noise");
    EXPECT_TRUE(sink.lines().empty());
    log.set_threshold(ops::Level::Debug);
    log.debug("noise");
    EXPECT_EQ(sink.lines().size(), 1U);
}

TEST_F(LoggerTest, AFieldThatDoesNotFitIsLeftOutAndTheLineStaysValid) {
    const std::string huge(2000, 'x');
    log.error("db", {{"first", 1}, {"message", huge}, {"after", 2}});
    const std::string line = sink.lines().at(0);
    EXPECT_LE(line.size() + 1, ops::LineBuilder::kCapacity);
    const auto doc = core::json::parse(line);
    ASSERT_TRUE(doc) << line;
    EXPECT_EQ(doc->find("first")->as_u64(), 1U);
    EXPECT_EQ(doc->find("message"), nullptr);
    EXPECT_EQ(doc->find("after"), nullptr);
    EXPECT_EQ(doc->find("truncated")->as_bool(), true);
}

TEST_F(LoggerTest, EscapingIsCountedWhenCheckingRoom) {
    // 300 control characters need 1800 bytes escaped, though they are 300 raw.
    const std::string controls(300, '\x02');
    log.error("db", {{"message", controls}});
    const auto doc = core::json::parse(sink.lines()[0]);
    ASSERT_TRUE(doc);
    EXPECT_EQ(doc->find("truncated")->as_bool(), true);
}

TEST_F(LoggerTest, AFieldThatFitsAsEscapedIsKeptWhateverItsWorstCase) {
    // 700 plain bytes fit beside the fixed fields; at six bytes each, the escaping's worst
    // case, they would not. The worker's "step failed" lost its error to that worst case.
    const std::string detail =
        "ffprobe exited 1: /" + std::string(660, 'p') + ": Permission denied";
    log.warn("step failed", {{"job", 1}, {"step", "probe"}, {"error", detail}, {"after", 2}});
    const auto doc = core::json::parse(sink.lines().at(0));
    ASSERT_TRUE(doc) << sink.lines().at(0);
    ASSERT_NE(doc->find("error"), nullptr);
    EXPECT_EQ(doc->find("error")->as_string(), detail);
    EXPECT_EQ(doc->find("after")->as_u64(), 2U);
    EXPECT_EQ(doc->find("truncated"), nullptr);
}

TEST_F(LoggerTest, AFieldExactlyAtTheRoomLeftFitsAndOneByteMoreDoesNot) {
    const std::string fixed = R"({"ts":"2026-01-01T01:02:03.045Z","level":"info","svc":"gateway",)"
                              R"("event":"e")";
    // The line keeps room for ,"truncated":true} and the newline.
    const std::size_t reserve = std::string_view(R"(,"truncated":true)").size() + 2;
    // ,"m":"" is 7 bytes beside the value.
    const std::size_t room = ops::LineBuilder::kCapacity - fixed.size() - reserve - 7;
    log.info("e", {{"m", std::string(room, 'a')}});
    log.info("e", {{"m", std::string(room + 1, 'a')}});
    const auto fits = core::json::parse(sink.lines().at(0));
    const auto over = core::json::parse(sink.lines().at(1));
    ASSERT_TRUE(fits && over);
    ASSERT_NE(fits->find("m"), nullptr);
    EXPECT_EQ(fits->find("m")->as_string(), std::string(room, 'a'));
    EXPECT_EQ(fits->find("truncated"), nullptr);
    EXPECT_EQ(over->find("m"), nullptr);
    EXPECT_EQ(over->find("truncated")->as_bool(), true);
}

TEST(LogLevel, NamesRoundTrip) {
    for (const auto level :
         {ops::Level::Debug, ops::Level::Info, ops::Level::Warn, ops::Level::Error}) {
        EXPECT_EQ(ops::parse_level(ops::to_string(level)), level);
    }
    EXPECT_FALSE(ops::parse_level("verbose"));
}

} // namespace
