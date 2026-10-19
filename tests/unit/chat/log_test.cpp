#include "core/util/json.hpp"

#include "log.hpp"

#include <gtest/gtest.h>
#include <regex>
#include <string>

namespace {

// Every line is one JSON object whose first member is the time, as the gateway's are (#192).
TEST(ChatLog, ALineStartsWithItsTimeToTheMillisecondInUtc) {
    ::testing::internal::CaptureStdout();
    chat::log_event(R"("level":"info","msg":"call","room":"{}","n":{})", "r-1", 3);
    const std::string out = ::testing::internal::GetCapturedStdout();
    ASSERT_FALSE(out.empty());
    EXPECT_EQ(out.back(), '\n');
    const std::string line = out.substr(0, out.size() - 1);
    EXPECT_TRUE(std::regex_match(
        line,
        std::regex(R"re(\{"ts":"\d{4}-\d{2}-\d{2}T\d{2}:\d{2}:\d{2}\.\d{3}Z","level":.*\})re")))
        << line;
    const auto json = core::json::parse(line);
    ASSERT_TRUE(json) << line;
    EXPECT_EQ(json->find("msg")->as_string(), "call");
    EXPECT_EQ(json->find("room")->as_string(), "r-1");
    EXPECT_EQ(json->find("n")->as_u64(), 3U);
}

} // namespace
