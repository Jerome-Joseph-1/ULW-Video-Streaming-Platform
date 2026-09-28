#include "control.hpp"

#include <string>
#include <thread>

namespace infra::storage::s3 {

std::expected<curl::Response, Failed> Control::send(curl::Method method,
                                                    const s3util::RequestTarget& target,
                                                    std::span<const s3util::Header> headers,
                                                    std::span<const std::byte> body,
                                                    std::size_t max_body) const {
    const std::string hash =
        body.empty() ? std::string(s3util::kEmptyPayloadSha256) : s3util::payload_sha256(body);
    auto result = curl::perform(endpoint_.sign(method, target, headers, hash, max_body), body);
    if (!result) {
        return std::unexpected(failed(result.error()));
    }
    if (!is_success(*result)) {
        return std::unexpected(failed(*result));
    }
    return std::move(*result);
}

bool Control::wait_before_retry(std::uint32_t retries_done, const Failed& failure) const {
    const auto delay =
        policy_.next_delay(retries_done, failure.error, failure.retry_after, random_);
    if (!delay) {
        return false;
    }
    // Control operations block by contract and run on pool threads, so sleeping here holds
    // up nothing but the caller that asked.
    std::this_thread::sleep_for(*delay);
    return true;
}

} // namespace infra::storage::s3
