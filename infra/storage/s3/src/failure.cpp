#include "failure.hpp"

#include "infra/s3util/errors.hpp"
#include "infra/s3util/retry.hpp"

#include <string_view>
#include <variant>

namespace infra::storage::s3 {

using core::ports::StorageError;

bool is_success(const curl::Response& response) noexcept {
    return response.status >= 200 && response.status <= 299;
}

Failed failed(const curl::Failure& failure) noexcept {
    switch (failure.kind) {
    case curl::FailureKind::Resolve:
    case curl::FailureKind::Connect:
    case curl::FailureKind::Timeout:
    case curl::FailureKind::Network:
        return {.error = StorageError::Transient, .retry_after = std::nullopt};
    case curl::FailureKind::Tls:
    case curl::FailureKind::BodyTooLarge:
    case curl::FailureKind::Local:
        return {.error = StorageError::Permanent, .retry_after = std::nullopt};
    }
    return {.error = StorageError::Permanent, .retry_after = std::nullopt};
}

Failed failed(const curl::Response& response) {
    const auto document = s3util::parse_error_document(response.body);
    const std::string_view code = document ? std::string_view(document->code) : std::string_view{};
    const auto retry_after = response.header("retry-after");
    return {.error = s3util::map_error(response.status, code).error,
            .retry_after = retry_after ? s3util::parse_retry_after(*retry_after) : std::nullopt};
}

Failed failed(const s3util::ResponseError& error) noexcept {
    if (const auto* document = std::get_if<s3util::ErrorDocument>(&error)) {
        constexpr int kArrivedWith = 200;
        return {.error = s3util::map_error(kArrivedWith, document->code).error,
                .retry_after = std::nullopt};
    }
    return {.error = StorageError::Transient, .retry_after = std::nullopt};
}

} // namespace infra::storage::s3
