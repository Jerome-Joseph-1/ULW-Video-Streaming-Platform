#include "codec/sdp/error.hpp"

#include <string_view>

namespace codec::sdp {

std::string_view message(ErrorCode code) noexcept {
    switch (code) {
    case ErrorCode::TooLarge:
        return "description too large";
    case ErrorCode::TooManyMediaSections:
        return "too many media sections";
    case ErrorCode::TooManyAttributes:
        return "too many attributes";
    case ErrorCode::TooManyFormats:
        return "too many formats";
    case ErrorCode::ForbiddenCharacter:
        return "NUL or stray CR";
    case ErrorCode::MalformedLine:
        return "malformed line";
    case ErrorCode::UnknownLineType:
        return "unknown line type";
    case ErrorCode::MisplacedLine:
        return "line out of order";
    case ErrorCode::MissingLine:
        return "required line missing";
    case ErrorCode::BadVersion:
        return "version is not 0";
    case ErrorCode::BadOrigin:
        return "malformed o=";
    case ErrorCode::BadSessionName:
        return "empty s=";
    case ErrorCode::BadConnection:
        return "malformed c=";
    case ErrorCode::BadBandwidth:
        return "malformed b=";
    case ErrorCode::BadTiming:
        return "malformed t=";
    case ErrorCode::BadMediaLine:
        return "malformed m=";
    case ErrorCode::DuplicateFormat:
        return "format listed twice";
    case ErrorCode::BadAttribute:
        return "malformed attribute";
    case ErrorCode::MisplacedAttribute:
        return "attribute at the wrong level";
    case ErrorCode::MissingMid:
        return "media section without mid";
    case ErrorCode::DuplicateMid:
        return "mid not unique";
    case ErrorCode::BadBundleGroup:
        return "inconsistent BUNDLE group";
    case ErrorCode::MissingIceCredentials:
        return "ice-ufrag or ice-pwd missing";
    case ErrorCode::MissingFingerprint:
        return "fingerprint missing";
    case ErrorCode::UnsupportedFingerprintAlgorithm:
        return "fingerprint algorithm not allowed";
    case ErrorCode::BadFingerprint:
        return "fingerprint length does not match its algorithm";
    case ErrorCode::MissingSetup:
        return "setup missing";
    case ErrorCode::UnknownPayloadType:
        return "payload type not on the m= line";
    case ErrorCode::DuplicatePayloadType:
        return "payload type mapped twice";
    case ErrorCode::DuplicateExtmapId:
        return "extmap id used twice";
    case ErrorCode::UnknownRid:
        return "simulcast names an undeclared rid";
    }
    return "unknown error";
}

} // namespace codec::sdp
