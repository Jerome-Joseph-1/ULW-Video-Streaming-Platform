#pragma once

#include "core/util/time.hpp"

#include <array>
#include <cstddef>
#include <cstdint>
#include <initializer_list>
#include <span>
#include <string>
#include <string_view>

namespace ops {

enum class MetricType : std::uint8_t { Counter, Gauge, Histogram };

struct Label {
    std::string_view name;
    std::string_view value;
};

// Durations in fixed buckets. Owned by one thread, like the counters of the shard it measures.
class Histogram {
public:
    static constexpr std::size_t kMaxBuckets = 16;

    // Upper bounds in seconds, ascending, at most kMaxBuckets; the +Inf bucket is implied.
    // Borrowed: pass an array with static storage.
    explicit Histogram(std::span<const double> bounds) noexcept;

    void observe(core::Millis duration) noexcept;

    [[nodiscard]] std::span<const double> bounds() const noexcept { return bounds_; }
    // Observations at or under each bound, not cumulative.
    [[nodiscard]] std::uint64_t bucket(std::size_t i) const noexcept { return counts_.at(i); }
    [[nodiscard]] std::uint64_t count() const noexcept { return count_; }
    [[nodiscard]] double sum_seconds() const noexcept { return sum_; }

private:
    std::span<const double> bounds_;
    std::array<std::uint64_t, kMaxBuckets> counts_{};
    std::uint64_t count_ = 0;
    double sum_ = 0.0;
};

// The Prometheus text exposition format, version 0.0.4: every family gets its HELP and TYPE
// lines before its samples.
class Exposition {
public:
    void family(std::string_view name, std::string_view help, MetricType type);
    void sample(std::string_view name, std::initializer_list<Label> labels, std::uint64_t value);
    void sample(std::string_view name, std::initializer_list<Label> labels, double value);

    void counter(std::string_view name, std::string_view help, std::uint64_t value);
    void gauge(std::string_view name, std::string_view help, std::uint64_t value);
    void gauge(std::string_view name, std::string_view help, double value);
    void histogram(std::string_view name, std::string_view help, const Histogram& h);

    [[nodiscard]] const std::string& text() const noexcept { return out_; }

private:
    void series(std::string_view name, std::string_view suffix,
                std::initializer_list<Label> labels);

    std::string out_;
};

} // namespace ops
