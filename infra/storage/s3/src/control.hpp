#pragma once

#include "core/ports/random.hpp"
#include "core/ports/storage.hpp"
#include "infra/curl/http.hpp"
#include "infra/s3util/retry.hpp"
#include "infra/s3util/sigv4.hpp"
#include "infra/s3util/url.hpp"

#include "endpoint.hpp"
#include "failure.hpp"

#include <cstddef>
#include <cstdint>
#include <expected>
#include <optional>
#include <span>
#include <type_traits>
#include <utility>

namespace infra::storage::s3 {

// Blocking requests for the control operations, which run on pool threads. Every request is
// signed over its real body hash and gets a handle of its own.
class Control {
public:
    Control(const Endpoint& endpoint, const s3util::RetryPolicy::Config& retry,
            core::ports::IRandom& random, PageCount& pages)
        : endpoint_(endpoint), policy_(retry), random_(random), pages_(pages) {}

    // One attempt: a 2xx response, or why there is none.
    [[nodiscard]] std::expected<curl::Response, Failed>
    send(curl::Method method, const s3util::RequestTarget& target,
         std::span<const s3util::Header> headers, std::span<const std::byte> body,
         std::size_t max_body) const;

    // Repeats `attempt` (returning std::expected<T, Failed>) until it succeeds, fails in a way
    // no retry can fix, or the policy gives up. Each attempt signs afresh, so a retry never
    // replays a stale x-amz-date.
    template <typename T, typename Attempt>
    [[nodiscard]] std::expected<T, core::ports::StorageError> retrying(Attempt attempt) const {
        for (std::uint32_t retries = 0;; ++retries) {
            std::expected<T, Failed> result = attempt();
            if (result) {
                if constexpr (std::is_void_v<T>) {
                    return {};
                } else {
                    return std::move(*result);
                }
            }
            pages_.note(result.error());
            if (!wait_before_retry(retries, result.error())) {
                return std::unexpected(result.error().error);
            }
        }
    }

private:
    // Sleeps out the backoff and returns true, or returns false to give up.
    [[nodiscard]] bool wait_before_retry(std::uint32_t retries_done, const Failed& failure) const;

    const Endpoint& endpoint_;
    s3util::RetryPolicy policy_;
    core::ports::IRandom& random_;
    PageCount& pages_;
};

} // namespace infra::storage::s3
