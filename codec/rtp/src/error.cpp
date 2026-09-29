#include "codec/rtp/error.hpp"

#include <string_view>

namespace codec::rtp {

std::string_view message(Error error) noexcept {
    switch (error) {
    case Error::Truncated:
        return "packet truncated";
    case Error::BadVersion:
        return "version is not 2";
    case Error::BadPadding:
        return "bad padding";
    case Error::BadExtension:
        return "header extension element overruns";
    case Error::BadReportCount:
        return "report count exceeds packet";
    case Error::BadSourceDescription:
        return "malformed SDES chunks";
    case Error::BadBye:
        return "malformed BYE";
    case Error::BadApp:
        return "APP shorter than its fixed fields";
    case Error::BadFeedback:
        return "malformed feedback message";
    }
    return "unknown error";
}

} // namespace codec::rtp
