#pragma once

#include "core/ports/clock.hpp"

#include <array>
#include <atomic>
#include <concepts>
#include <cstddef>
#include <cstdint>
#include <initializer_list>
#include <optional>
#include <string_view>
#include <variant>

namespace ops {

enum class Level : std::uint8_t { Debug, Info, Warn, Error };

[[nodiscard]] std::optional<Level> parse_level(std::string_view text) noexcept;
[[nodiscard]] std::string_view to_string(Level level) noexcept;

// One key and value of a log line. Values are borrowed: a Field lives only for the call that
// writes it.
class Field {
public:
    template <class T>
        requires std::convertible_to<const T&, std::string_view>
    Field(std::string_view key, const T& value) noexcept
        : key_(key), value_(std::string_view(value)) {}
    Field(std::string_view key, bool value) noexcept : key_(key), value_(value) {}
    template <std::integral T>
        requires(!std::same_as<T, bool>)
    Field(std::string_view key, T value) noexcept : key_(key), value_(widen(value)) {}
    Field(std::string_view key, double value) noexcept : key_(key), value_(value) {}

    [[nodiscard]] std::string_view key() const noexcept { return key_; }
    [[nodiscard]] const auto& value() const noexcept { return value_; }

private:
    template <std::integral T> static constexpr auto widen(T value) noexcept {
        if constexpr (std::is_signed_v<T>) {
            return static_cast<std::int64_t>(value);
        } else {
            return static_cast<std::uint64_t>(value);
        }
    }

    std::string_view key_;
    std::variant<std::string_view, std::int64_t, std::uint64_t, bool, double> value_;
};

// A JSON object on one line, built in place. A field that does not fit is left out and the
// line says so, so a long error message can cost its own detail but never the line.
class LineBuilder {
public:
    // A request line is about 300 bytes; the rest is room for a database's error message.
    static constexpr std::size_t kCapacity = 1024;

    LineBuilder(core::WallTime at, Level level, std::string_view service,
                std::string_view event) noexcept;
    void add(const Field& field) noexcept;
    // The finished line, newline included; valid until the builder goes.
    [[nodiscard]] std::string_view finish() noexcept;

private:
    [[nodiscard]] bool fits(std::size_t n) const noexcept;
    void raw(std::string_view text) noexcept;
    void quoted(std::string_view text) noexcept;

    std::array<char, kCapacity> buf_{};
    std::size_t len_ = 0;
    bool truncated_ = false;
};

class ILogSink {
public:
    virtual ~ILogSink() = default;
    // Takes a finished line, from any thread.
    virtual void write(std::string_view line) noexcept = 0;
    // Lines lost because the sink could not keep up.
    [[nodiscard]] virtual std::uint64_t dropped() const noexcept = 0;
};

// Lines straight to stdout, one write each. A full pipe blocks the caller, which is fine for
// the worker, whose threads block by design, and for any process before its event loop runs.
class StdoutSink final : public ILogSink {
public:
    StdoutSink() noexcept = default;
    // Another descriptor, such as stderr for what must still be said once stdout's reader has
    // stalled.
    explicit StdoutSink(int fd) noexcept : fd_(fd) {}

    void write(std::string_view line) noexcept override;
    [[nodiscard]] std::uint64_t dropped() const noexcept override { return 0; }

private:
    int fd_ = 1;
};

// JSON lines: {"ts":...,"level":...,"svc":...,"event":...,<fields>}. Event names and field
// keys are fixed text chosen at the call site; request bodies, tokens, keys, signatures and
// signed URLs are never passed as values.
class Logger {
public:
    Logger(ILogSink& sink, const core::ports::IClock& clock, std::string_view service,
           Level threshold) noexcept
        : sink_(sink), clock_(clock), service_(service), threshold_(threshold) {}

    [[nodiscard]] bool enabled(Level level) const noexcept { return level >= threshold_.load(); }
    // The threshold comes from configuration, which is only read after the logger exists.
    void set_threshold(Level level) noexcept { threshold_.store(level); }
    [[nodiscard]] std::uint64_t dropped() const noexcept { return sink_.dropped(); }

    void log(Level level, std::string_view event, std::initializer_list<Field> fields) noexcept;
    void debug(std::string_view event, std::initializer_list<Field> fields = {}) noexcept {
        log(Level::Debug, event, fields);
    }
    void info(std::string_view event, std::initializer_list<Field> fields = {}) noexcept {
        log(Level::Info, event, fields);
    }
    void warn(std::string_view event, std::initializer_list<Field> fields = {}) noexcept {
        log(Level::Warn, event, fields);
    }
    void error(std::string_view event, std::initializer_list<Field> fields = {}) noexcept {
        log(Level::Error, event, fields);
    }

private:
    ILogSink& sink_;
    const core::ports::IClock& clock_;
    std::string_view service_;
    std::atomic<Level> threshold_;
};

} // namespace ops
