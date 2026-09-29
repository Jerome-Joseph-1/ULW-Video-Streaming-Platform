#pragma once

#include <cstdint>
#include <string_view>

namespace codec::sdp {

enum class ErrorCode : std::uint8_t {
    TooLarge,
    TooManyMediaSections,
    TooManyAttributes,
    TooManyFormats,
    ForbiddenCharacter,
    MalformedLine,
    UnknownLineType,
    MisplacedLine,
    MissingLine,
    BadVersion,
    BadOrigin,
    BadSessionName,
    BadConnection,
    BadBandwidth,
    BadTiming,
    BadMediaLine,
    DuplicateFormat,
    BadAttribute,
    MisplacedAttribute,
    MissingMid,
    DuplicateMid,
    BadBundleGroup,
    MissingIceCredentials,
    MissingFingerprint,
    UnsupportedFingerprintAlgorithm,
    BadFingerprint,
    MissingSetup,
    UnknownPayloadType,
    DuplicatePayloadType,
    DuplicateExtmapId,
    UnknownRid,
};

struct Error {
    ErrorCode code;
    // 1-based; 0 when the description as a whole is at fault.
    std::uint32_t line;

    friend bool operator==(const Error&, const Error&) = default;
};

[[nodiscard]] std::string_view message(ErrorCode code) noexcept;

} // namespace codec::sdp
