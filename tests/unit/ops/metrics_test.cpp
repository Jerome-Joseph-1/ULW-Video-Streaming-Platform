#include "ops/metrics.hpp"

#include <array>
#include <gtest/gtest.h>
#include <limits>

namespace {

constexpr std::array kBounds{0.1, 1.0, 10.0};

TEST(Exposition, EveryFamilyGetsHelpAndTypeBeforeItsSamples) {
    ops::Exposition e;
    e.counter("requests_total", "Requests parsed.", 7);
    e.family("timeouts_total", "Connections ended by a timer.", ops::MetricType::Counter);
    e.sample("timeouts_total", {{.name = "kind", .value = "header"}}, std::uint64_t{2});
    e.gauge("open_fds", "Descriptors open.", std::uint64_t{12});
    EXPECT_EQ(e.text(), "# HELP requests_total Requests parsed.\n"
                        "# TYPE requests_total counter\n"
                        "requests_total 7\n"
                        "# HELP timeouts_total Connections ended by a timer.\n"
                        "# TYPE timeouts_total counter\n"
                        "timeouts_total{kind=\"header\"} 2\n"
                        "# HELP open_fds Descriptors open.\n"
                        "# TYPE open_fds gauge\n"
                        "open_fds 12\n");
}

TEST(Exposition, LabelValuesAndHelpAreEscaped) {
    ops::Exposition e;
    e.family("x", "a\\b\nc", ops::MetricType::Gauge);
    e.sample("x", {{.name = "v", .value = "q\"\\\n"}}, std::uint64_t{1});
    EXPECT_EQ(e.text(), "# HELP x a\\\\b\\nc\n# TYPE x gauge\nx{v=\"q\\\"\\\\\\n\"} 1\n");
}

TEST(Exposition, UnknownAndInfiniteValuesAreSpelledAsPrometheusReadsThem) {
    ops::Exposition e;
    e.gauge("a", "h", std::numeric_limits<double>::quiet_NaN());
    e.gauge("b", "h", std::numeric_limits<double>::infinity());
    EXPECT_NE(e.text().find("\na NaN\n"), std::string::npos) << e.text();
    EXPECT_NE(e.text().find("\nb +Inf\n"), std::string::npos) << e.text();
}

TEST(Histogram, BucketsAreCumulativeAndEndInInf) {
    ops::Histogram h(kBounds);
    h.observe(core::Millis{50});
    h.observe(core::Millis{100});
    h.observe(core::Millis{2'000});
    h.observe(core::Millis{60'000});
    ops::Exposition e;
    e.histogram("part_upload_duration_seconds", "Chunk time.", h);
    EXPECT_EQ(e.text(), "# HELP part_upload_duration_seconds Chunk time.\n"
                        "# TYPE part_upload_duration_seconds histogram\n"
                        "part_upload_duration_seconds_bucket{le=\"0.1\"} 2\n"
                        "part_upload_duration_seconds_bucket{le=\"1\"} 2\n"
                        "part_upload_duration_seconds_bucket{le=\"10\"} 3\n"
                        "part_upload_duration_seconds_bucket{le=\"+Inf\"} 4\n"
                        "part_upload_duration_seconds_sum 62.15\n"
                        "part_upload_duration_seconds_count 4\n");
}

} // namespace
