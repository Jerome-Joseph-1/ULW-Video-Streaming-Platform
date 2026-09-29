#include "core/ports/media.hpp"

#include "room_service.hpp"

#include <gtest/gtest.h>
#include <optional>

namespace {

using core::ports::MediaError;
using infra::curl::Failure;
using infra::curl::FailureKind;
using infra::curl::Response;
using infra::curl::Result;
using infra::sfu::livekit::detail::classify;
using infra::sfu::livekit::detail::IfAbsent;

Result status(int code) {
    return Response{.status = code, .headers = {}, .body = {}};
}

Result failure(FailureKind kind) {
    return std::unexpected(Failure{.kind = kind, .detail = {}});
}

std::optional<MediaError> error_of(const Result& r, IfAbsent absent = IfAbsent::Fail) {
    const auto outcome = classify(r, absent);
    return outcome ? std::nullopt : std::optional(outcome.error());
}

TEST(Classify, AnyTwoHundredIsSuccess) {
    EXPECT_EQ(error_of(status(200)), std::nullopt);
    EXPECT_EQ(error_of(status(204)), std::nullopt);
}

TEST(Classify, NotFoundSucceedsOnlyWhereAbsenceIsTheGoal) {
    EXPECT_EQ(error_of(status(404), IfAbsent::Succeed), std::nullopt);
    EXPECT_EQ(error_of(status(404), IfAbsent::Fail), MediaError::Refused);
    // Absence forgives nothing else.
    EXPECT_EQ(error_of(status(403), IfAbsent::Succeed), MediaError::Refused);
}

TEST(Classify, ClientErrorsAreRefusedAndServerErrorsRetryable) {
    for (const int code : {400, 401, 403, 409, 412}) {
        EXPECT_EQ(error_of(status(code)), MediaError::Refused) << code;
    }
    for (const int code : {408, 429, 500, 502, 503}) {
        EXPECT_EQ(error_of(status(code)), MediaError::Unavailable) << code;
    }
}

TEST(Classify, TransportFailuresAreRetryableExceptWhereConfigurationIsWrong) {
    for (const auto kind : {FailureKind::Resolve, FailureKind::Connect, FailureKind::Timeout,
                            FailureKind::Network, FailureKind::BodyTooLarge}) {
        EXPECT_EQ(error_of(failure(kind)), MediaError::Unavailable);
    }
    EXPECT_EQ(error_of(failure(FailureKind::Tls)), MediaError::Refused);
    EXPECT_EQ(error_of(failure(FailureKind::Local)), MediaError::Refused);
}

} // namespace
