#pragma once

#include "core/models/ids.hpp"
#include "core/ports/clock.hpp"
#include "core/ports/notify.hpp"
#include "core/ports/random.hpp"
#include "net/reactor.hpp"

#include "chat_service.hpp"
#include "notify_sender.hpp"
#include "ring.hpp"

#include <cstddef>
#include <cstdint>
#include <functional>
#include <map>
#include <memory>
#include <optional>
#include <set>
#include <string>
#include <unordered_map>
#include <vector>

// The notification hook's events (ADR-0111): which devices of whom have a socket open on this
// node, written to the connectivity registry; a call's events on its room's owner; and who is
// owed word of a message, on the node its sender is on. Everything runs on the reactor thread, and
// nothing waits on it: a ring, a ticket and a message go on whatever the registry or the receiver
// do.
namespace chat {

struct NotifyLimits {
    // How often this node renews its lease (NotifyTiming::lease is 60 s: three tries per lease).
    core::Millis renew_every{20'000};
    // A change of this node's sockets is written this long after it, with every change that came
    // meanwhile: a page reload opens and closes within it and costs no write.
    core::Millis flush_after{250};
    // A write that failed is tried again after this.
    core::Millis retry_after{1'000};
    // A call_ringing whose registry lookup takes longer goes out with `devices` unknown.
    core::Millis lookup_timeout{1'000};
    // Calls whose call_ringing is waiting on the registry; past it, one goes out at once with
    // `devices` unknown.
    std::size_t max_lookups = 1'024;
    // Message lookups in flight; a message past it is not looked up, and counted.
    std::size_t max_message_lookups = 256;
};

struct NotifyCounters {
    std::uint64_t renewals = 0;
    std::uint64_t renew_failures = 0;
    // The registry had forgotten this node (it was down for NotifyTiming::forget_after, or it
    // is the node's first renewal): every socket was written again.
    std::uint64_t resyncs = 0;
    std::uint64_t syncs = 0;
    std::uint64_t sync_failures = 0;
    // call_ringing sent with `devices` unknown: the lookup failed, timed out, or could not be
    // asked.
    std::uint64_t lookups_unknown = 0;
    // Messages that told nobody because the lookup failed or could not be asked.
    std::uint64_t message_failures = 0;
    // Events handed to the sender.
    std::uint64_t events = 0;
    std::uint64_t allocation_failures = 0;
};

class Notifier final : public IRingWatcher, public IMessageWatcher, public net::ITimerHandler {
public:
    // `node` is this run's tag in the registry: random, unique per run.
    Notifier(net::IReactor& reactor, core::ports::IConnectivityStore& store, IEventSink& sink,
             const core::ports::IClock& clock, core::ports::IRandom& random, std::uint64_t node,
             NotifyLimits limits = {});
    ~Notifier() override;
    Notifier(const Notifier&) = delete;
    Notifier& operator=(const Notifier&) = delete;
    Notifier(Notifier&&) = delete;
    Notifier& operator=(Notifier&&) = delete;

    // Renews the lease, now and every renew_every.
    void start() noexcept;
    // The drain: no more writes; this node's rows are deleted, and `done` is called once they
    // are, or the store gave up.
    void stop(std::move_only_function<void() noexcept> done) noexcept;

    // A socket of `user`'s opened or closed here, naming `device` when it connected.
    void opened(const core::UserId& user, const std::optional<core::DeviceId>& device) noexcept;
    void closed(const core::UserId& user, const std::optional<core::DeviceId>& device) noexcept;

    // A message `sender` sent here was sequenced `seq` in the room.
    void message(const core::RoomId& room, const core::UserId& sender,
                 std::uint64_t seq) noexcept override;

    void observed(const CallEvent& event) noexcept override;
    void on_timeout() noexcept override;

    [[nodiscard]] const NotifyCounters& counters() const noexcept { return counters_; }
    // Distinct (user, device) pairs with a socket here, and those the registry may not have yet.
    [[nodiscard]] std::size_t sockets() const noexcept;
    [[nodiscard]] std::size_t unwritten() const noexcept { return dirty_.size(); }
    [[nodiscard]] bool renewed() const noexcept { return renewed_; }

private:
    struct Socket {
        core::ports::SocketDevice who;
        std::uint32_t open = 0;
        // What the registry was last told.
        bool written = false;
        // What the sync in flight tells it.
        std::optional<bool> writing;
    };
    // A call whose call_ringing waits on the registry, with the events of the call that came
    // since, to follow it in order.
    struct Lookup final : net::ITimerHandler {
        Lookup(Notifier& owner, CallEvent ringing) : notifier(owner), event(std::move(ringing)) {}
        void on_timeout() noexcept override;
        Notifier& notifier;
        CallEvent event;
        std::vector<std::string> after;
        net::TimerId timer;
    };

    [[nodiscard]] static std::string key_of(const core::ports::SocketDevice& who);
    void changed(const core::UserId& user, const std::optional<core::DeviceId>& device,
                 bool open) noexcept;
    void renew() noexcept;
    void renewed(core::ports::NotifyResult<bool> result) noexcept;
    void sync() noexcept;
    void synced(std::uint64_t generation, core::ports::NotifyResult<void> result) noexcept;
    // The one timer: the next renewal, or a write due sooner.
    void arm() noexcept;
    void flush_soon(core::Millis after) noexcept;

    // Sends call_ringing with what the registry answered (nullopt: unknown), then what waited.
    void ring_out(const CallId& call,
                  const std::optional<std::vector<core::ports::SocketDevice>>& devices) noexcept;
    [[nodiscard]] std::string
    ringing_text(const CallEvent& event,
                 const std::optional<std::vector<core::ports::SocketDevice>>& devices);
    void offer(std::string event) noexcept;
    void head(std::string& out, std::string_view type);

    net::IReactor& reactor_;
    core::ports::IConnectivityStore& store_;
    IEventSink& sink_;
    const core::ports::IClock& clock_;
    core::ports::IRandom& random_;
    std::uint64_t node_;
    NotifyLimits limits_;
    NotifyCounters counters_;
    // Store answers that come after the notifier is gone find this expired.
    std::shared_ptr<Notifier*> self_;

    std::map<std::string, Socket> sockets_;
    std::set<std::string> dirty_;
    bool started_ = false;
    bool stopped_ = false;
    bool renewed_ = false;
    bool renewing_ = false;
    bool syncing_ = false;
    // The keys the sync in flight writes.
    std::vector<std::string> writing_;
    // Bumped when the registry forgot this node: a sync asked before is not taken.
    std::uint64_t generation_ = 0;
    core::MonoTime next_renew_;
    std::optional<core::MonoTime> flush_at_;
    net::TimerId timer_;
    core::MonoTime timer_due_;

    std::unordered_map<CallId, std::unique_ptr<Lookup>> lookups_;
    std::size_t message_lookups_ = 0;
};

} // namespace chat
