// The signalling side of tests/call, until the call handler carries it over the room WebSocket:
// each run performs one SFU port operation against the LiveKit server and prints what the
// handler would send the client.
#include "core/models/ids.hpp"
#include "core/ports/media.hpp"
#include "core/util/json.hpp"
#include "infra/curl/multi.hpp"
#include "infra/sfu/livekit/livekit_sfu.hpp"
#include "net/reactor_factory.hpp"
#include "os/system_clock.hpp"

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <expected>
#include <memory>
#include <optional>
#include <print>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace {

constexpr std::string_view kUsage = R"(usage:
  ulw_call_harness open <room-id>
  ulw_call_harness join <room-id> <user> <device-id>
      Opens the room if need be and prints the participant's ticket as JSON.
  ulw_call_harness remove <room-id> <user> <device-id>
  ulw_call_harness close <room-id>
LIVEKIT_API_KEY and LIVEKIT_API_SECRET are required; LIVEKIT_API_URL defaults to
http://127.0.0.1:7880 and LIVEKIT_CLIENT_URL to ws://127.0.0.1:7880.
)";

constexpr int kUsageError = 2;
// A 1:1 call.
constexpr std::uint16_t kCallParticipants = 2;
// The harness holds one libcurl connection and the reactor's own descriptors.
constexpr std::size_t kMaxFds = 64;

int fail(std::string_view what) {
    std::println(stderr, "ulw_call_harness: {}", what);
    return 1;
}

int usage() {
    std::print(stderr, "{}", kUsage);
    return kUsageError;
}

std::string env_or(const char* name, std::string_view fallback) {
    // Read once, before anything else could call setenv.
    // NOLINTNEXTLINE(concurrency-mt-unsafe)
    const char* value = std::getenv(name);
    return value != nullptr ? std::string(value) : std::string(fallback);
}

struct Harness {
    os::SystemClock clock;
    std::unique_ptr<net::IReactor> reactor;
    std::unique_ptr<infra::curl::Multi> multi;
    std::unique_ptr<core::ports::ISfu> sfu;

    template <class Pred> void run_until(Pred done) const {
        while (!done()) {
            reactor->run_once(core::Millis{100});
        }
    }

    [[nodiscard]] std::expected<std::unique_ptr<core::ports::IMediaRoom>, core::ports::MediaError>
    open(const core::RoomId& room) const {
        std::optional<
            std::expected<std::unique_ptr<core::ports::IMediaRoom>, core::ports::MediaError>>
            opened;
        sfu->open_room(room, kCallParticipants,
                       [&](auto result) noexcept { opened = std::move(result); });
        run_until([&] { return opened.has_value(); });
        return std::move(*opened);
    }

    template <class Start>
    [[nodiscard]] std::expected<void, core::ports::MediaError> wait(Start start) const {
        std::optional<std::expected<void, core::ports::MediaError>> outcome;
        start([&](std::expected<void, core::ports::MediaError> r) noexcept { outcome = r; });
        run_until([&] { return outcome.has_value(); });
        return *outcome;
    }
};

// On the heap: the reactor and the adapter keep references to the clock and to each other.
std::unique_ptr<Harness> make_harness() {
    auto harness = std::make_unique<Harness>();
    Harness& h = *harness;
    auto choice = net::make_reactor_with_fallback(net::ReactorKind::IoUring, h.clock, kMaxFds);
    if (!choice) {
        fail("cannot create a reactor");
        return nullptr;
    }
    h.reactor = std::move(choice->reactor);
    auto multi = infra::curl::Multi::create(*h.reactor);
    if (!multi) {
        fail("cannot start libcurl");
        return nullptr;
    }
    h.multi = std::move(*multi);
    auto sfu = infra::sfu::livekit::make_sfu(
        *h.reactor, *h.multi, h.clock,
        infra::sfu::livekit::Config{.api_url = env_or("LIVEKIT_API_URL", "http://127.0.0.1:7880"),
                                    .client_url =
                                        env_or("LIVEKIT_CLIENT_URL", "ws://127.0.0.1:7880"),
                                    .api_key = env_or("LIVEKIT_API_KEY", ""),
                                    .api_secret = env_or("LIVEKIT_API_SECRET", "")});
    if (!sfu) {
        fail(infra::sfu::livekit::to_string(sfu.error()));
        return nullptr;
    }
    h.sfu = std::move(*sfu);
    return harness;
}

int run(std::span<const std::string_view> args) {
    if (args.empty()) {
        return usage();
    }
    const std::string_view command = args[0];
    const bool names_participant = command == "join" || command == "remove";
    if (args.size() != (names_participant ? 4U : 2U)) {
        return usage();
    }
    const auto room_id = core::RoomId::parse(args[1]);
    if (!room_id) {
        return fail("room id is not a UUID");
    }
    std::optional<core::UserId> user;
    std::optional<core::DeviceId> device;
    if (names_participant) {
        auto u = core::UserId::parse(args[2]);
        auto d = core::DeviceId::parse(args[3]);
        if (!u || !d) {
            return fail("user or device id malformed");
        }
        user = *u;
        device = *d;
    }
    if (command != "open" && command != "close" && !names_participant) {
        return usage();
    }

    const std::unique_ptr<Harness> h = make_harness();
    if (!h) {
        return 1;
    }
    auto room = h->open(*room_id);
    if (!room) {
        return fail(core::ports::to_string(room.error()));
    }
    if (command == "open") {
        return 0;
    }
    if (command == "close") {
        const auto closed =
            h->wait([&](core::ports::MediaDone done) { (*room)->close(std::move(done)); });
        return closed ? 0 : fail(core::ports::to_string(closed.error()));
    }
    auto participant = (*room)->join(*user, *device);
    if (!participant) {
        return fail(core::ports::to_string(participant.error()));
    }
    if (command == "remove") {
        const auto removed =
            h->wait([&](core::ports::MediaDone done) { (*participant)->remove(std::move(done)); });
        return removed ? 0 : fail(core::ports::to_string(removed.error()));
    }
    const core::ports::MediaTicket& ticket = (*participant)->ticket();
    std::string out = R"({"url":)";
    core::json::append_string(out, ticket.endpoint);
    out += R"(,"token":)";
    core::json::append_string(out, ticket.credential);
    out += R"(,"expires_at":)";
    out += std::to_string(
        std::chrono::floor<std::chrono::seconds>(ticket.expires_at).time_since_epoch().count());
    out += '}';
    std::println("{}", out);
    return 0;
}

} // namespace

int main(int argc, char** argv) try {
    const std::span<char*> raw(argv, static_cast<std::size_t>(argc));
    const std::vector<std::string_view> args(raw.begin() + (raw.empty() ? 0 : 1), raw.end());
    return run(args);
} catch (...) {
    static_cast<void>(std::fputs("ulw_call_harness: failed\n", stderr));
    return 1;
}
