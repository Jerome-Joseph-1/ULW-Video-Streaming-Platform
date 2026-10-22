#include "notify.hpp"

#include "core/ports/message_store.hpp"
#include "core/util/json.hpp"
#include "core/util/uuid.hpp"
#include "rt/room_store.hpp"

#include <algorithm>
#include <chrono>
#include <format>
#include <iterator>
#include <new>
#include <utility>

namespace chat {

namespace {

using core::ports::NotifyResult;
using core::ports::SocketDevice;

std::string_view type_of(RingEvent event) noexcept {
    switch (event) {
    case RingEvent::Ringing:
        return "call_ringing";
    case RingEvent::Answered:
        return "call_answered";
    case RingEvent::Declined:
        return "call_declined";
    case RingEvent::Cancelled:
        return "call_cancelled";
    case RingEvent::Missed:
        return "call_missed";
    case RingEvent::Ended:
        return "call_ended";
    case RingEvent::Left:
        return "call_left";
    case RingEvent::Moved:
        return "call_moved";
    }
    return "call_ended";
}

void append_user(std::string& out, const core::UserId& user) {
    core::json::append_string(out, user.view());
}

void append_users(std::string& out, const std::vector<core::UserId>& users) {
    out += '[';
    for (std::size_t i = 0; i < users.size(); ++i) {
        if (i != 0) {
            out += ',';
        }
        append_user(out, users[i]);
    }
    out += ']';
}

std::int64_t unix_seconds(core::WallTime t) noexcept {
    return std::chrono::duration_cast<core::Seconds>(t.time_since_epoch()).count();
}

// The call's fields every call event has.
void append_call(std::string& out, const CallEvent& event) {
    out += R"(,"room":")";
    out += event.room.to_string();
    out += R"(","call":")";
    out += event.call.to_string();
    out += R"(","kind":")";
    out += event.kind == CallKind::Group ? "group" : "direct";
    out += R"(","caller":)";
    append_user(out, event.caller);
}

} // namespace

Notifier::Notifier(net::IReactor& reactor, core::ports::IConnectivityStore& store, IEventSink& sink,
                   const core::ports::IClock& clock, core::ports::IRandom& random,
                   std::uint64_t node, NotifyLimits limits)
    : reactor_(reactor), store_(store), sink_(sink), clock_(clock), random_(random), node_(node),
      limits_(limits), self_(std::make_shared<Notifier*>(this)), next_renew_(clock.now()) {}

Notifier::~Notifier() {
    reactor_.cancel_timer(timer_);
    for (auto& [call, lookup] : lookups_) {
        reactor_.cancel_timer(lookup->timer);
    }
}

std::size_t Notifier::sockets() const noexcept {
    return static_cast<std::size_t>(
        std::ranges::count_if(sockets_, [](const auto& entry) { return entry.second.open > 0; }));
}

std::string Notifier::key_of(const SocketDevice& who) {
    std::string key(who.user.view());
    key += '/';
    if (who.device) {
        key += who.device->to_string();
    }
    return key;
}

void Notifier::start() noexcept {
    if (started_ || stopped_) {
        return;
    }
    started_ = true;
    renew();
}

void Notifier::stop(std::move_only_function<void() noexcept> done) noexcept {
    if (stopped_) {
        done();
        return;
    }
    stopped_ = true;
    reactor_.cancel_timer(timer_);
    timer_ = {};
    // What waits on the registry goes now, devices unknown: the node is going.
    while (!lookups_.empty()) {
        ++counters_.lookups_unknown;
        ring_out(lookups_.begin()->first, std::nullopt);
    }
    store_.forget(node_, [done = std::move(done)](NotifyResult<void>) mutable noexcept { done(); });
}

void Notifier::opened(const core::UserId& user,
                      const std::optional<core::DeviceId>& device) noexcept {
    changed(user, device, true);
}

void Notifier::closed(const core::UserId& user,
                      const std::optional<core::DeviceId>& device) noexcept {
    changed(user, device, false);
}

void Notifier::changed(const core::UserId& user, const std::optional<core::DeviceId>& device,
                       bool open) noexcept {
    if (stopped_) {
        return;
    }
    try {
        const SocketDevice who{.user = user, .device = device};
        std::string key = key_of(who);
        auto [it, made] = sockets_.try_emplace(
            key, Socket{.who = who, .open = 0, .written = false, .writing = std::nullopt});
        Socket& s = it->second;
        if (open) {
            ++s.open;
        } else if (s.open > 0) {
            --s.open;
        }
        dirty_.insert(std::move(key));
        flush_soon(limits_.flush_after);
    } catch (const std::bad_alloc&) {
        // The registry misses this socket until the next resync: a push it did not need, or
        // none it did.
        ++counters_.allocation_failures;
    }
}

void Notifier::flush_soon(core::Millis after) noexcept {
    const core::MonoTime at = clock_.now() + after;
    if (!flush_at_ || at < *flush_at_) {
        flush_at_ = at;
    }
    arm();
}

void Notifier::arm() noexcept {
    if (stopped_ || !started_) {
        return;
    }
    core::MonoTime due = next_renew_;
    if (flush_at_) {
        due = std::min(due, *flush_at_);
    }
    if (timer_ != net::TimerId{}) {
        if (timer_due_ == due) {
            return;
        }
        reactor_.cancel_timer(timer_);
        timer_ = {};
    }
    const core::MonoTime now = clock_.now();
    const auto delay = due <= now ? core::Millis{0} : std::chrono::ceil<core::Millis>(due - now);
    try {
        timer_ = reactor_.arm_timer(delay, *this);
        timer_due_ = due;
    } catch (const std::bad_alloc&) {
        ++counters_.allocation_failures;
    }
}

void Notifier::on_timeout() noexcept {
    timer_ = {};
    const core::MonoTime now = clock_.now();
    if (now >= next_renew_ && !renewing_) {
        renew();
    }
    if (flush_at_ && now >= *flush_at_) {
        flush_at_.reset();
        sync();
    }
    arm();
}

void Notifier::renew() noexcept {
    if (stopped_) {
        return;
    }
    renewing_ = true;
    next_renew_ = clock_.now() + limits_.renew_every;
    store_.renew(node_, [weak = std::weak_ptr(self_)](NotifyResult<bool> result) mutable noexcept {
        if (const auto self = weak.lock()) {
            (*self)->renewed(result);
        }
    });
    arm();
}

void Notifier::renewed(NotifyResult<bool> result) noexcept {
    renewing_ = false;
    if (stopped_) {
        return;
    }
    if (!result) {
        ++counters_.renew_failures;
        // Tried again sooner than a renewal: a lease is three renewals long.
        next_renew_ = std::min(next_renew_, clock_.now() + limits_.retry_after);
        arm();
        return;
    }
    ++counters_.renewals;
    const bool first = !renewed_;
    renewed_ = true;
    if (*result) {
        // The registry has no row of this run: every socket is written again. A sync in flight
        // wrote to the rows that were forgotten; its answer is not taken.
        ++counters_.resyncs;
        ++generation_;
        try {
            std::erase_if(sockets_, [](const auto& entry) { return entry.second.open == 0; });
            for (auto& [key, s] : sockets_) {
                s.written = false;
                s.writing.reset();
                dirty_.insert(key);
            }
        } catch (const std::bad_alloc&) {
            ++counters_.allocation_failures;
        }
    }
    if (first || *result || !dirty_.empty()) {
        flush_soon(core::Millis{0});
    }
    arm();
}

void Notifier::sync() noexcept {
    if (!renewed_ || syncing_ || stopped_ || dirty_.empty()) {
        return;
    }
    try {
        std::vector<SocketDevice> added;
        std::vector<SocketDevice> removed;
        std::vector<std::string> keys;
        for (auto it = dirty_.begin();
             it != dirty_.end() && added.size() + removed.size() < core::ports::kMaxSyncRows;) {
            const auto s = sockets_.find(*it);
            if (s == sockets_.end()) {
                it = dirty_.erase(it);
                continue;
            }
            const bool open = s->second.open > 0;
            if (open == s->second.written) {
                if (!open) {
                    sockets_.erase(s);
                }
                it = dirty_.erase(it);
                continue;
            }
            (open ? added : removed).push_back(s->second.who);
            s->second.writing = open;
            keys.push_back(*it);
            ++it;
        }
        if (keys.empty()) {
            return;
        }
        syncing_ = true;
        writing_ = std::move(keys);
        store_.sync(node_, std::move(added), std::move(removed),
                    [weak = std::weak_ptr(self_),
                     generation = generation_](NotifyResult<void> result) mutable noexcept {
                        if (const auto self = weak.lock()) {
                            (*self)->synced(generation, result);
                        }
                    });
    } catch (const std::bad_alloc&) {
        ++counters_.allocation_failures;
        for (auto& [key, s] : sockets_) {
            s.writing.reset();
        }
        writing_.clear();
        syncing_ = false;
        flush_soon(limits_.retry_after);
    }
}

void Notifier::synced(std::uint64_t generation, NotifyResult<void> result) noexcept {
    syncing_ = false;
    const std::vector<std::string> keys = std::exchange(writing_, {});
    if (stopped_) {
        return;
    }
    const bool taken = result.has_value() && generation == generation_;
    if (result) {
        ++counters_.syncs;
    } else {
        ++counters_.sync_failures;
    }
    for (const std::string& key : keys) {
        const auto s = sockets_.find(key);
        if (s == sockets_.end()) {
            continue;
        }
        const std::optional<bool> wrote = s->second.writing;
        if (!wrote) {
            continue;
        }
        if (taken) {
            s->second.written = *wrote;
        }
        s->second.writing.reset();
        const bool open = s->second.open > 0;
        if (open == s->second.written) {
            dirty_.erase(key);
            if (!open) {
                sockets_.erase(s);
            }
        }
    }
    if (!dirty_.empty()) {
        flush_soon(result ? core::Millis{0} : limits_.retry_after);
    }
}

void Notifier::head(std::string& out, std::string_view type) {
    out += R"({"id":")";
    out += core::Uuid::v7(clock_, random_).to_string();
    out += R"(","type":")";
    out += type;
    std::format_to(std::back_inserter(out), R"(","at":{})", unix_seconds(clock_.wall_now()));
}

void Notifier::offer(std::string event) noexcept {
    ++counters_.events;
    sink_.offer(std::move(event));
}

void Notifier::message(const core::RoomId& room, const core::UserId& sender,
                       std::uint64_t seq) noexcept {
    // A stream's live chat has no member list to tell, and thousands of senders.
    if (stopped_ || core::ports::is_stream_chat(room)) {
        return;
    }
    if (message_lookups_ >= limits_.max_message_lookups) {
        ++counters_.message_failures;
        return;
    }
    ++message_lookups_;
    if (limits_.message_present) {
        store_.message_audience(
            room, sender,
            [weak = std::weak_ptr(self_), room, sender,
             seq](NotifyResult<core::ports::MessageAudience> result) mutable noexcept {
                const auto self = weak.lock();
                if (!self) {
                    return;
                }
                Notifier& n = **self;
                --n.message_lookups_;
                if (!result) {
                    ++n.counters_.message_failures;
                    return;
                }
                if (result->away.empty() && result->present.empty()) {
                    return;
                }
                try {
                    n.offer(n.message_text(room, sender, seq, *result));
                } catch (...) {
                    ++n.counters_.allocation_failures;
                }
            });
        return;
    }
    store_.message_targets(room, sender,
                           [weak = std::weak_ptr(self_), room, sender,
                            seq](NotifyResult<std::vector<core::UserId>> result) mutable noexcept {
                               const auto self = weak.lock();
                               if (!self) {
                                   return;
                               }
                               Notifier& n = **self;
                               --n.message_lookups_;
                               if (!result) {
                                   ++n.counters_.message_failures;
                                   return;
                               }
                               if (result->empty()) {
                                   return;
                               }
                               try {
                                   std::string out;
                                   n.head(out, "message");
                                   out += R"(,"room":")";
                                   out += room.to_string();
                                   out += R"(","sender":)";
                                   append_user(out, sender);
                                   std::format_to(std::back_inserter(out), R"(,"seq":{},"users":)",
                                                  seq);
                                   append_users(out, *result);
                                   out += '}';
                                   n.offer(std::move(out));
                               } catch (...) {
                                   // std::bad_alloc, or std::format's own error, which this
                                   // format cannot raise.
                                   ++n.counters_.allocation_failures;
                               }
                           });
}

// Every message, as its owner sequenced it, with what the backend checks a claim against (who sent
// which seq in which room, under which id) and never its body (ADR-0133). Not looked up, not
// throttled, and from the owner alone, so a seq is told once whichever node its sender is on.
void Notifier::sequenced(const rt::Message& message) noexcept {
    // A presence room keeps nothing, and a stream's live chat has thousands of senders whose
    // events would crowd every other one out of the queue.
    if (!limits_.sequenced || stopped_ || rt::is_ephemeral_room(message.room) ||
        core::ports::is_stream_chat(message.room)) {
        return;
    }
    try {
        std::string out;
        head(out, "message_sequenced");
        out += R"(,"room":")";
        out += message.room.to_string();
        std::format_to(std::back_inserter(out), R"(","seq":{},"sender":)", message.seq);
        append_user(out, message.sender);
        out += R"(,"message_id":)";
        core::json::append_string(out, message.key.view());
        // Unix milliseconds, floored, as the socket's `at` (ADR-0112).
        std::format_to(
            std::back_inserter(out), R"(,"sent_at":{},"quiet":{}}})",
            std::chrono::floor<std::chrono::milliseconds>(message.sent_at.time_since_epoch())
                .count(),
            message.quiet);
        offer(std::move(out));
    } catch (...) {
        // std::bad_alloc, or std::format's own error, which this format cannot raise.
        ++counters_.allocation_failures;
    }
}

// "users": those away, as without the option; "present": each member with a socket in view and
// the devices those sockets named, so that the receiver pushes to their other devices
// (ADR-0126).
std::string Notifier::message_text(const core::RoomId& room, const core::UserId& sender,
                                   std::uint64_t seq,
                                   const core::ports::MessageAudience& audience) {
    std::string out;
    head(out, "message");
    out += R"(,"room":")";
    out += room.to_string();
    out += R"(","sender":)";
    append_user(out, sender);
    std::format_to(std::back_inserter(out), R"(,"seq":{},"users":)", seq);
    append_users(out, audience.away);
    out += R"(,"present":[)";
    for (std::size_t i = 0; i < audience.present.size(); ++i) {
        const SocketDevice& socket = audience.present[i];
        const bool first = i == 0 || audience.present[i - 1].user != socket.user;
        if (first) {
            if (i != 0) {
                out += "]},";
            }
            out += R"({"user":)";
            append_user(out, socket.user);
            out += R"(,"devices":[)";
        }
        if (socket.device) {
            if (out.back() != '[') {
                out += ',';
            }
            out += '"';
            out += socket.device->to_string();
            out += '"';
        }
    }
    if (!audience.present.empty()) {
        out += "]}";
    }
    out += "]}";
    return out;
}

void Notifier::observed(const CallEvent& event) noexcept {
    if (stopped_) {
        return;
    }
    try {
        if (event.event == RingEvent::Ringing) {
            if (lookups_.contains(event.call)) {
                return;
            }
            if (lookups_.size() >= limits_.max_lookups) {
                ++counters_.lookups_unknown;
                offer(ringing_text(event, std::nullopt));
                return;
            }
            auto lookup = std::make_unique<Lookup>(*this, event);
            Lookup& l = *lookup;
            lookups_.emplace(event.call, std::move(lookup));
            l.timer = reactor_.arm_timer(limits_.lookup_timeout, l);
            std::vector<core::UserId> users = event.users;
            if (users.size() > core::ports::kMaxConnectedQuery) {
                users.erase(users.begin() +
                                static_cast<std::ptrdiff_t>(core::ports::kMaxConnectedQuery),
                            users.end());
            }
            store_.connected(std::move(users),
                             [weak = std::weak_ptr(self_), call = event.call](
                                 NotifyResult<std::vector<SocketDevice>> result) mutable noexcept {
                                 const auto self = weak.lock();
                                 if (!self || !(*self)->lookups_.contains(call)) {
                                     return;
                                 }
                                 Notifier& n = **self;
                                 if (!result) {
                                     ++n.counters_.lookups_unknown;
                                     n.ring_out(call, std::nullopt);
                                     return;
                                 }
                                 n.ring_out(call, *result);
                             });
            return;
        }
        std::string out;
        head(out, type_of(event.event));
        append_call(out, event);
        out += R"(,"by":)";
        if (event.by) {
            append_user(out, *event.by);
        } else {
            out += "null";
        }
        if (event.event == RingEvent::Missed) {
            out += R"(,"users":)";
            append_users(out, event.users);
        }
        if (event.reason) {
            out += R"(,"reason":")";
            out += reason_name(*event.reason);
            out += '"';
        }
        if (event.event == RingEvent::Declined && !event.text.empty()) {
            out += R"(,"text":)";
            core::json::append_string(out, event.text);
        }
        out += '}';
        // After the call's call_ringing, which may still wait on the registry.
        if (const auto it = lookups_.find(event.call); it != lookups_.end()) {
            it->second->after.push_back(std::move(out));
            return;
        }
        offer(std::move(out));
    } catch (...) {
        // std::bad_alloc, or std::format's own error, which these formats cannot raise.
        ++counters_.allocation_failures;
    }
}

std::string Notifier::ringing_text(const CallEvent& event,
                                   const std::optional<std::vector<SocketDevice>>& devices) {
    std::string out;
    head(out, "call_ringing");
    append_call(out, event);
    std::format_to(std::back_inserter(out), R"(,"expires_at":{},"callees":[)",
                   unix_seconds(event.expires_at));
    for (std::size_t i = 0; i < event.users.size(); ++i) {
        const core::UserId& user = event.users[i];
        if (i != 0) {
            out += ',';
        }
        out += R"({"user":)";
        append_user(out, user);
        if (!devices) {
            out += R"(,"connected":null,"devices":null})";
            continue;
        }
        bool connected = false;
        std::string listed;
        for (const SocketDevice& s : *devices) {
            if (s.user != user) {
                continue;
            }
            connected = true;
            if (s.device) {
                listed += listed.empty() ? "\"" : ",\"";
                listed += s.device->to_string();
                listed += '"';
            }
        }
        out += R"(,"connected":)";
        out += connected ? "true" : "false";
        out += R"(,"devices":[)";
        out += listed;
        out += "]}";
    }
    out += "]}";
    return out;
}

void Notifier::ring_out(const CallId& call,
                        const std::optional<std::vector<SocketDevice>>& devices) noexcept {
    const auto it = lookups_.find(call);
    if (it == lookups_.end()) {
        return;
    }
    std::unique_ptr<Lookup> lookup = std::move(it->second);
    lookups_.erase(it);
    reactor_.cancel_timer(lookup->timer);
    try {
        offer(ringing_text(lookup->event, devices));
    } catch (const std::bad_alloc&) {
        ++counters_.allocation_failures;
    }
    for (std::string& event : lookup->after) {
        offer(std::move(event));
    }
}

void Notifier::Lookup::on_timeout() noexcept {
    timer = {};
    ++notifier.counters_.lookups_unknown;
    // Destroys this lookup: nothing of it is touched after.
    notifier.ring_out(event.call, std::nullopt);
}

} // namespace chat
