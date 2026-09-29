#pragma once

#include <cstdint>
#include <string_view>

namespace codec::rtp {

enum class Error : std::uint8_t {
    Truncated,
    BadVersion,
    BadPadding,
    BadExtension,
    BadReportCount,
    BadSourceDescription,
    BadBye,
    BadApp,
    BadFeedback,
};

[[nodiscard]] std::string_view message(Error error) noexcept;

} // namespace codec::rtp
