#pragma once

#include "core/ports/clock.hpp"
#include "core/ports/media.hpp"
#include "infra/curl/multi.hpp"
#include "net/reactor.hpp"

#include "access_token.hpp"

#include <expected>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

namespace infra::sfu::livekit::detail {

enum class IfAbsent : std::uint8_t {
    Fail,
    // For removals: LiveKit answers not_found for a room or participant that is already gone,
    // which is the state the caller asked for.
    Succeed,
};

// LiveKit's error codes travel as Twirp codes mapped onto HTTP statuses; the status alone
// separates what a retry can fix from what it cannot.
[[nodiscard]] std::expected<void, core::ports::MediaError> classify(const curl::Result& result,
                                                                    IfAbsent absent) noexcept;

// What a call answered: its body on success.
using Answer = std::expected<std::string, core::ports::MediaError>;
using AnswerDone = std::move_only_function<void(Answer) noexcept>;

struct CallLimits {
    core::Millis timeout;
    std::size_t max_response;
};

// Calls to LiveKit's server API (RoomService, and Egress for live streams), each authorised by
// a token minted for that call alone. Reactor thread only.
class RoomService {
public:
    RoomService(net::IReactor& reactor, curl::Multi& multi, const core::ports::IClock& clock,
                std::string api_url, ApiKey key) noexcept;
    // Pending calls are cancelled; their callbacks never run.
    ~RoomService();
    RoomService(const RoomService&) = delete;
    RoomService& operator=(const RoomService&) = delete;

    // POSTs the JSON `body` to `method`, "<service>/<method>" of LiveKit's Twirp services.
    // `done` runs later on the reactor thread, failures that happen before anything is sent
    // included.
    void call(std::string_view method, std::string body, const Grant& grant, IfAbsent absent,
              core::ports::MediaDone done);
    // As call, for a method whose answer the caller reads, within its own limits.
    void fetch(std::string_view method, std::string body, const Grant& grant,
               const CallLimits& limits, AnswerDone done);

    // `done` runs later with `error`, and nothing is sent.
    void fail(core::ports::MediaError error, AnswerDone done);

    [[nodiscard]] const ApiKey& key() const noexcept { return key_; }
    [[nodiscard]] const core::ports::IClock& clock() const noexcept { return clock_; }

private:
    class Call;

    void start(std::string_view method, std::string body, const Grant& grant, IfAbsent absent,
               const CallLimits& limits, AnswerDone done);
    void finished(Call& call, Answer outcome) noexcept;

    net::IReactor& reactor_;
    curl::Multi& multi_;
    const core::ports::IClock& clock_;
    std::string api_url_;
    ApiKey key_;
    std::vector<std::unique_ptr<Call>> calls_;
};

} // namespace infra::sfu::livekit::detail
