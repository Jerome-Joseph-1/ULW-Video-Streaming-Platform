#pragma once

#include <expected>
#include <string>
#include <string_view>

namespace live {

// Names a stream in the store's keys and in logs, so it is one key segment of letters, digits,
// '_' and '-', short enough to leave room in the 1024-byte key limit.
class StreamId {
public:
    static constexpr std::size_t kMaxLength = 64;

    [[nodiscard]] static std::expected<StreamId, std::string> parse(std::string_view text);

    [[nodiscard]] const std::string& str() const noexcept { return value_; }
    // Every object of the stream lives under this prefix.
    [[nodiscard]] std::string key_prefix() const { return "live/" + value_ + "/"; }

private:
    explicit StreamId(std::string value) : value_(std::move(value)) {}
    std::string value_;
};

} // namespace live
