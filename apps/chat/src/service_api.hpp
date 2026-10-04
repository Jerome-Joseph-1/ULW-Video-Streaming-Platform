#pragma once

#include "core/ports/auth.hpp"
#include "core/ports/clock.hpp"
#include "core/ports/message_store.hpp"
#include "core/util/time.hpp"
#include "net/reactor.hpp"
#include "os/unique_fd.hpp"

#include "token_bucket.hpp"

#include <cstddef>
#include <cstdint>
#include <memory>
#include <string_view>
#include <vector>

namespace chat {

// Where every path of the service API starts.
inline constexpr std::string_view kServicePrefix = "/service/v1/";

struct ServiceApiLimits {
    // The operator's backend keeps a few connections alive, one request at a time on each; past
    // this many open at once a new one is closed at accept.
    std::size_t max_connections = 32;
    // An idle keep-alive connection is closed after this; a request must be in, head and body,
    // this long after its first byte.
    core::Millis idle_timeout{30'000};
    core::Millis request_timeout{10'000};
    // The largest request: 50 user ids of 128 bytes, quoted, are under 7 KiB.
    std::size_t max_body = std::size_t{16} * 1024;
    // Requests per node, from every connection together. A backend that lists the two sides of
    // an accepted request, or a new group of 50, makes one; 100 at once and 50 a second, per
    // chat node, is far past a product's sign-ups and invitations, and keeps a runaway script to
    // a few hundred transactions a second across a three-node deployment, each holding one
    // room's row for milliseconds.
    std::uint32_t burst = 100;
    std::uint32_t per_second = 50;
};

struct ServiceApiCounters {
    std::uint64_t connections = 0;
    // Closed at accept: max_connections were open, or the reactor would not take the socket.
    std::uint64_t refused_connections = 0;
    std::uint64_t requests = 0;
    // Answered with what was asked: a change made (or found made), or a listing.
    std::uint64_t changes = 0;
    std::uint64_t reads = 0;
    // No bearer token, or one that fails verification (401); a valid token without the service
    // claim (403).
    std::uint64_t unauthorized = 0;
    std::uint64_t forbidden = 0;
    // Over the rate (429).
    std::uint64_t limited = 0;
    // A request that is not one of the API's: path, method, body (4xx other than the above).
    std::uint64_t bad_requests = 0;
    // A change the store refused (409, or 404 for a room nothing recorded).
    std::uint64_t refused = 0;
    // The store or the key set could not be reached (503).
    std::uint64_t unavailable = 0;
};

// Chat's service API (ADR-0096): member lists managed by the operator's backend rather than by
// users, for a product that decides itself who may talk to whom. Plain HTTP/1.1 on a listener of
// its own (ULW_SERVICE_PORT), POST with a JSON body, answered in JSON. Each request carries the
// identity provider's token as `Authorization: Bearer`, verified as a user's is, and must carry
// the service claim (ULW_SERVICE_CLAIM and ULW_SERVICE_SCOPE; core::ports::Claims::is_service); a
// cookie is never read. Changes go through the store's member-list changes, under the same lock,
// kinds, caps and notifications as the users' commands; the rooms are the ones those commands name.
//
// One request at a time per connection: the next is read once the answer is out. Reactor thread
// only. The store's answers must not reach a destroyed ServiceApi: destroy the store first.
class ServiceApi final : public net::IAcceptHandler {
public:
    ServiceApi(net::IReactor& reactor, const core::ports::IClock& clock,
               core::ports::IJwtVerifier& verifier, core::ports::IMessageStore& store,
               ServiceApiLimits limits);
    ~ServiceApi() override;
    ServiceApi(const ServiceApi&) = delete;
    ServiceApi& operator=(const ServiceApi&) = delete;
    ServiceApi(ServiceApi&&) = delete;
    ServiceApi& operator=(ServiceApi&&) = delete;

    void on_accept(os::UniqueFd conn) noexcept override;
    // Destroys connections the reactor has let go of. Call after each run_once.
    void reap() noexcept;
    // Closes every connection and accepts no more: for a drain.
    void stop() noexcept;

    [[nodiscard]] const ServiceApiCounters& counters() const noexcept { return counters_; }
    [[nodiscard]] std::size_t connections() const noexcept { return connections_.size(); }

private:
    class Connection;

    [[nodiscard]] Connection* find(std::uint64_t id) noexcept;

    net::IReactor& reactor_;
    const core::ports::IClock& clock_;
    core::ports::IJwtVerifier& verifier_;
    core::ports::IMessageStore& store_;
    ServiceApiLimits limits_;
    ServiceApiCounters counters_;
    TokenBucket bucket_;
    std::uint64_t next_id_ = 1;
    bool stopped_ = false;
    std::vector<std::unique_ptr<Connection>> connections_;
};

} // namespace chat
