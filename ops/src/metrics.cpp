#include "ops/metrics.hpp"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <format>
#include <iterator>

namespace ops {

namespace {

std::string_view type_name(MetricType type) noexcept {
    switch (type) {
    case MetricType::Counter:
        return "counter";
    case MetricType::Gauge:
        return "gauge";
    case MetricType::Histogram:
        return "histogram";
    }
    return "untyped";
}

// Label values escape backslash, double quote and newline; HELP text only backslash and
// newline.
void escaped(std::string& out, std::string_view text, bool quote) {
    for (const char c : text) {
        if (c == '\\') {
            out += "\\\\";
        } else if (c == '\n') {
            out += "\\n";
        } else if (quote && c == '"') {
            out += "\\\"";
        } else {
            out += c;
        }
    }
}

} // namespace

Histogram::Histogram(std::span<const double> bounds) noexcept
    : bounds_(bounds.first(std::min(bounds.size(), kMaxBuckets))) {}

void Histogram::observe(core::Millis duration) noexcept {
    const double seconds = std::chrono::duration<double>(duration).count();
    ++count_;
    sum_ += seconds;
    const auto it = std::ranges::lower_bound(bounds_, seconds);
    if (it != bounds_.end()) {
        ++counts_.at(static_cast<std::size_t>(it - bounds_.begin()));
    }
}

void Exposition::family(std::string_view name, std::string_view help, MetricType type) {
    out_ += "# HELP ";
    out_ += name;
    out_ += ' ';
    escaped(out_, help, false);
    std::format_to(std::back_inserter(out_), "\n# TYPE {} {}\n", name, type_name(type));
}

void Exposition::series(std::string_view name, std::string_view suffix,
                        std::initializer_list<Label> labels) {
    out_ += name;
    out_ += suffix;
    if (labels.size() != 0) {
        out_ += '{';
        bool first = true;
        for (const Label& l : labels) {
            if (!first) {
                out_ += ',';
            }
            first = false;
            out_ += l.name;
            out_ += "=\"";
            escaped(out_, l.value, true);
            out_ += '"';
        }
        out_ += '}';
    }
    out_ += ' ';
}

void Exposition::sample(std::string_view name, std::initializer_list<Label> labels,
                        std::uint64_t value) {
    series(name, {}, labels);
    std::format_to(std::back_inserter(out_), "{}\n", value);
}

void Exposition::sample(std::string_view name, std::initializer_list<Label> labels, double value) {
    series(name, {}, labels);
    // The format spells these as Go does; std::format would write nan and inf.
    if (std::isnan(value)) {
        out_ += "NaN\n";
    } else if (std::isinf(value)) {
        out_ += value > 0 ? "+Inf\n" : "-Inf\n";
    } else {
        std::format_to(std::back_inserter(out_), "{}\n", value);
    }
}

void Exposition::counter(std::string_view name, std::string_view help, std::uint64_t value) {
    family(name, help, MetricType::Counter);
    sample(name, {}, value);
}

void Exposition::gauge(std::string_view name, std::string_view help, std::uint64_t value) {
    family(name, help, MetricType::Gauge);
    sample(name, {}, value);
}

void Exposition::gauge(std::string_view name, std::string_view help, double value) {
    family(name, help, MetricType::Gauge);
    sample(name, {}, value);
}

void Exposition::histogram(std::string_view name, std::string_view help, const Histogram& h) {
    family(name, help, MetricType::Histogram);
    std::uint64_t cumulative = 0;
    for (std::size_t i = 0; i < h.bounds().size(); ++i) {
        cumulative += h.bucket(i);
        series(name, "_bucket", {{.name = "le", .value = std::format("{}", h.bounds()[i])}});
        std::format_to(std::back_inserter(out_), "{}\n", cumulative);
    }
    series(name, "_bucket", {{.name = "le", .value = "+Inf"}});
    std::format_to(std::back_inserter(out_), "{}\n", h.count());
    series(name, "_sum", {});
    std::format_to(std::back_inserter(out_), "{}\n", h.sum_seconds());
    series(name, "_count", {});
    std::format_to(std::back_inserter(out_), "{}\n", h.count());
}

} // namespace ops
