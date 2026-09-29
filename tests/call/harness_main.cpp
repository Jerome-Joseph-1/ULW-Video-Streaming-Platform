// The signalling side of tests/call, until the call handler carries it over the room WebSocket.
// Like the handler, it is one long-lived process that keeps the rooms it opened: it reads one
// command per line on stdin, runs it through the SFU port, and answers with one line on stdout.
//
//   open <room-id> <generation> <max-participants>   -> ok
//   join <room-id> <generation> <user> <device-id> <member|publisher>  -> the ticket, as JSON
//   relay <room-id> <generation> <user> <device-id> <keyframe-seconds> <srt-url>  -> ok
//   close <room-id> <generation>                      -> ok
//
// A command that fails answers "error <reason>". LIVEKIT_API_KEY and LIVEKIT_API_SECRET are
// required; LIVEKIT_API_URL defaults to http://127.0.0.1:7880 and LIVEKIT_CLIENT_URL to
// ws://127.0.0.1:7880.
#include "core/models/ids.hpp"
#include "core/ports/media.hpp"
#include "core/util/json.hpp"
#include "infra/curl/multi.hpp"
#include "infra/sfu/livekit/livekit_sfu.hpp"
#include "net/reactor_factory.hpp"
#include "os/system_clock.hpp"

#include <charconv>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <expected>
#include <iostream>
#include <limits>
#include <map>
#include <memory>
#include <optional>
#include <print>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>
#include <vector>

namespace {

using core::ports::IMediaRoom;
using core::ports::MediaError;
using core::ports::MediaGeneration;

// The harness holds one libcurl connection and the reactor's own descriptors.
constexpr std::size_t kMaxFds = 64;
// Twice the adapter's 5 s request timeout, which libcurl does not apply to a transfer still
// queued for a connection.
constexpr std::chrono::seconds kCommandDeadline{10};

std::string env_or(const char* name, std::string_view fallback) {
    // Read once, before anything else could call setenv.
    // NOLINTNEXTLINE(concurrency-mt-unsafe)
    const char* value = std::getenv(name);
    return value != nullptr ? std::string(value) : std::string(fallback);
}

template <class T> std::optional<T> parse_number(std::string_view text) {
    std::uint64_t value = 0;
    const auto [end, ec] = std::from_chars(text.data(), text.data() + text.size(), value);
    if (ec != std::errc{} || end != text.data() + text.size() ||
        value > std::numeric_limits<T>::max()) {
        return std::nullopt;
    }
    return static_cast<T>(value);
}

std::vector<std::string_view> split(std::string_view line) {
    std::vector<std::string_view> words;
    while (!line.empty()) {
        const auto space = line.find(' ');
        if (space != 0) {
            words.push_back(line.substr(0, space));
        }
        line = space == std::string_view::npos ? std::string_view{} : line.substr(space + 1);
    }
    return words;
}

std::string ticket_json(const core::ports::MediaTicket& ticket) {
    std::string out = R"({"url":)";
    core::json::append_string(out, ticket.endpoint);
    out += R"(,"token":)";
    core::json::append_string(out, ticket.credential);
    out += R"(,"expires_at":)";
    out += std::to_string(
        std::chrono::floor<std::chrono::seconds>(ticket.expires_at).time_since_epoch().count());
    out += '}';
    return out;
}

class Harness {
public:
    // On the heap: the reactor and the adapter keep references to the clock and to each other.
    static std::unique_ptr<Harness> create() {
        auto h = std::make_unique<Harness>();
        auto choice =
            net::make_reactor_with_fallback(net::ReactorKind::IoUring, h->clock_, kMaxFds);
        if (!choice) {
            std::println(stderr, "ulw_call_harness: cannot create a reactor");
            return nullptr;
        }
        h->reactor_ = std::move(choice->reactor);
        auto multi = infra::curl::Multi::create(*h->reactor_);
        if (!multi) {
            std::println(stderr, "ulw_call_harness: cannot start libcurl");
            return nullptr;
        }
        h->multi_ = std::move(*multi);
        auto sfu = infra::sfu::livekit::make_sfu(
            *h->reactor_, *h->multi_, h->clock_,
            infra::sfu::livekit::Config{
                .api_url = env_or("LIVEKIT_API_URL", "http://127.0.0.1:7880"),
                .client_url = env_or("LIVEKIT_CLIENT_URL", "ws://127.0.0.1:7880"),
                .api_key = env_or("LIVEKIT_API_KEY", ""),
                .api_secret = env_or("LIVEKIT_API_SECRET", "")});
        if (!sfu) {
            std::println(stderr, "ulw_call_harness: {}",
                         infra::sfu::livekit::to_string(sfu.error()));
            return nullptr;
        }
        h->sfu_ = std::move(*sfu);
        return h;
    }

    // The answer line for one command line.
    std::string run(std::string_view line) {
        const std::vector<std::string_view> words = split(line);
        if (words.size() < 3) {
            return "error usage";
        }
        const auto room = core::RoomId::parse(words[1]);
        const auto generation = parse_number<std::uint64_t>(words[2]);
        if (!room || !generation) {
            return "error bad room id or generation";
        }
        std::string key(words[1]);
        key += ':';
        key += words[2];
        if (words[0] == "open" && words.size() == 4) {
            const auto max = parse_number<std::uint16_t>(words[3]);
            return max ? open(*room, MediaGeneration{*generation}, *max, std::move(key))
                       : "error bad max-participants";
        }
        if (words[0] == "join" && words.size() == 6) {
            const auto user = core::UserId::parse(words[3]);
            const auto device = core::DeviceId::parse(words[4]);
            if (!user || !device || (words[5] != "member" && words[5] != "publisher")) {
                return "error bad user, device id or role";
            }
            return join(key, *user, *device,
                        words[5] == "member" ? core::ports::MediaRole::Member
                                             : core::ports::MediaRole::Publisher);
        }
        if (words[0] == "relay" && words.size() == 7) {
            const auto user = core::UserId::parse(words[3]);
            const auto device = core::DeviceId::parse(words[4]);
            const auto keyframes = parse_number<std::uint32_t>(words[5]);
            if (!user || !device || !keyframes) {
                return "error bad user, device id or keyframe interval";
            }
            return relay(key, *user, *device,
                         core::ports::MediaRelay{.url = std::string(words[6]),
                                                 .keyframe_interval = core::Seconds{*keyframes}});
        }
        if (words[0] == "close" && words.size() == 3) {
            return close(key);
        }
        return "error usage";
    }

private:
    std::string open(const core::RoomId& room, MediaGeneration generation, std::uint16_t max,
                     std::string key) {
        std::optional<std::expected<std::unique_ptr<IMediaRoom>, MediaError>> opened;
        sfu_->open_room(room, generation, max,
                        [&](auto result) noexcept { opened = std::move(result); });
        run_until([&] { return opened.has_value(); });
        if (!*opened) {
            return "error " + std::string(to_string(opened->error()));
        }
        rooms_.insert_or_assign(std::move(key), std::move(**opened));
        return "ok";
    }

    std::string join(const std::string& key, const core::UserId& user, const core::DeviceId& device,
                     core::ports::MediaRole role) {
        const auto room = rooms_.find(key);
        if (room == rooms_.end()) {
            return "error not open";
        }
        std::optional<std::expected<core::ports::MediaTicket, MediaError>> ticket;
        room->second->join(user, device, role,
                           [&](auto result) noexcept { ticket = std::move(result); });
        run_until([&] { return ticket.has_value(); });
        return *ticket ? ticket_json(**ticket) : "error " + std::string(to_string(ticket->error()));
    }

    std::string relay(const std::string& key, const core::UserId& user,
                      const core::DeviceId& device, const core::ports::MediaRelay& target) {
        const auto room = rooms_.find(key);
        if (room == rooms_.end()) {
            return "error not open";
        }
        std::optional<std::expected<void, MediaError>> relayed;
        room->second->relay(user, device, target,
                            [&](std::expected<void, MediaError> r) noexcept { relayed = r; });
        run_until([&] { return relayed.has_value(); });
        return *relayed ? "ok" : "error " + std::string(to_string(relayed->error()));
    }

    std::string close(const std::string& key) {
        const auto room = rooms_.find(key);
        if (room == rooms_.end()) {
            return "error not open";
        }
        std::optional<std::expected<void, MediaError>> closed;
        room->second->close([&](std::expected<void, MediaError> r) noexcept { closed = r; });
        run_until([&] { return closed.has_value(); });
        rooms_.erase(room);
        return *closed ? "ok" : "error " + std::string(to_string(closed->error()));
    }

    // A command still unanswered at its deadline ends the process: its callback, which points
    // into this command's frame, may yet run.
    template <class Pred> void run_until(Pred done) {
        const auto deadline = std::chrono::steady_clock::now() + kCommandDeadline;
        while (!done()) {
            if (std::chrono::steady_clock::now() > deadline) {
                std::println(stderr, "ulw_call_harness: command timed out");
                std::_Exit(1);
            }
            reactor_->run_once(core::Millis{100});
        }
    }

    // Declared in dependency order, so rooms go first and the reactor last.
    os::SystemClock clock_;
    std::unique_ptr<net::IReactor> reactor_;
    std::unique_ptr<infra::curl::Multi> multi_;
    std::unique_ptr<core::ports::ISfu> sfu_;
    std::map<std::string, std::unique_ptr<IMediaRoom>> rooms_;
};

} // namespace

int main() try {
    const std::unique_ptr<Harness> harness = Harness::create();
    if (!harness) {
        return 1;
    }
    std::string line;
    while (std::getline(std::cin, line)) {
        std::println("{}", harness->run(line));
        static_cast<void>(std::fflush(stdout));
    }
    return 0;
} catch (...) {
    static_cast<void>(std::fputs("ulw_call_harness: failed\n", stderr));
    return 1;
}
