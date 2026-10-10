#include "key_directory.hpp"

#include <algorithm>
#include <new>
#include <string>
#include <utility>

namespace chat {

using core::ports::DeviceEntry;
using core::ports::E2eeError;
using core::ports::E2eeResult;
using core::ports::FetchedKeyPackage;

// One claim_key_packages: the devices it takes a package of, and what each gave, until the last
// fetch has answered.
struct KeyDirectory::Claim {
    DirectoryClientId client;
    core::UserId user;
    std::optional<rt::MessageKey> id;
    std::vector<core::DeviceId> devices;
    std::size_t pending = 0;
    ClaimOutcome outcome;
};

namespace {

// What a refusal of the directory's says to the client.
std::string_view reason_for(E2eeError error, bool registering) noexcept {
    switch (error) {
    case E2eeError::NotFound:
        // Another user's device reads as unknown, registration included (ADR-0038).
        return "unknown_device";
    case E2eeError::Revoked:
        return "device_retired";
    case E2eeError::Full:
        return registering ? "device_limit" : "key_packages_full";
    case E2eeError::Invalid:
        return "malformed";
    case E2eeError::Exhausted:
    case E2eeError::StaleEpoch:
    case E2eeError::Unavailable:
    case E2eeError::Corrupt:
        return "unavailable";
    }
    return "unavailable";
}

} // namespace

KeyDirectory::KeyDirectory(core::ports::IDeviceRegistry* registry,
                           core::ports::IE2eeDeliveryService* delivery, IPresenceAccess& access,
                           const core::ports::IClock& clock, DirectoryLimits limits)
    : registry_(registry), delivery_(delivery), access_(access), clock_(clock), limits_(limits),
      self_(std::make_shared<KeyDirectory*>(this)) {}

KeyDirectory::~KeyDirectory() = default;

DirectoryClientId KeyDirectory::attach(IClient& client, const core::UserId& user) {
    const DirectoryClientId id{next_client_++};
    clients_.emplace(id.value,
                     Client{.client = &client, .user = user, .in_flight = 0, .devices = {}});
    ++user_entry(user).clients;
    return id;
}

void KeyDirectory::detach(DirectoryClientId id) noexcept {
    const auto it = clients_.find(id.value);
    if (it == clients_.end()) {
        return;
    }
    if (const auto u = users_.find(it->second.user); u != users_.end() && u->second.clients > 0) {
        --u->second.clients;
    }
    clients_.erase(it);
    prune();
}

KeyDirectory::Client* KeyDirectory::find(DirectoryClientId id) noexcept {
    const auto it = clients_.find(id.value);
    return it == clients_.end() ? nullptr : &it->second;
}

KeyDirectory::User& KeyDirectory::user_entry(const core::UserId& user) {
    const core::MonoTime now = clock_.now();
    return users_
        .try_emplace(user,
                     User{.clients = 0,
                          .reads = TokenBucket(limits_.read_burst, limits_.reads_per_second, now),
                          .writes = PacedBucket(limits_.write_burst, limits_.write_interval, now)})
        .first->second;
}

// Entries of users with no connection here are kept while their allowances are not full again,
// so reconnecting does not refill them; past that they are worth nothing. Looked at only once
// the table holds well more entries than connections.
void KeyDirectory::prune() noexcept {
    constexpr std::size_t kSlack = 64;
    if (users_.size() <= (clients_.size() * 2) + kSlack) {
        return;
    }
    const core::MonoTime now = clock_.now();
    std::erase_if(users_, [&](const auto& entry) {
        const User& u = entry.second;
        return u.clients == 0 && u.reads.full(now) && u.writes.full(now);
    });
}

void KeyDirectory::push(Client& c, std::string_view text) noexcept {
    static_cast<void>(c.client->push(text));
}

void KeyDirectory::refuse(Client& c, std::string_view reason,
                          const DirectoryErrorContext& context) noexcept {
    try {
        std::string out;
        write_directory_error(out, reason, context);
        push(c, out);
    } catch (const std::bad_alloc&) {
        c.client->allocation_failed();
    }
}

void KeyDirectory::refuse_for(Client& c, E2eeError error,
                              const DirectoryErrorContext& context) noexcept {
    const std::string_view reason = reason_for(error, false);
    if (reason == "unknown_device") {
        ++counters_.unknown_device;
    } else if (reason == "device_retired") {
        ++counters_.device_retired;
    } else if (reason == "key_packages_full") {
        ++counters_.key_packages_full;
    } else if (reason == "unavailable") {
        ++counters_.unavailable;
    }
    refuse(c, reason, context);
}

bool KeyDirectory::admit(Client& c, bool write, const DirectoryErrorContext& context) {
    if (registry_ == nullptr || delivery_ == nullptr) {
        ++counters_.unavailable;
        refuse(c, "unavailable", context);
        return false;
    }
    if (c.in_flight >= limits_.max_in_flight) {
        ++counters_.busy;
        refuse(c, "busy", context);
        return false;
    }
    User& u = user_entry(c.user);
    const core::MonoTime now = clock_.now();
    std::optional<core::Millis> wait;
    if (write) {
        if (u.writes.available(now)) {
            u.writes.take(now);
        } else {
            wait = u.writes.wait(now);
        }
    } else if (auto taken = u.reads.take(now); !taken) {
        wait = taken.error();
    }
    if (wait) {
        ++counters_.rate_limited;
        DirectoryErrorContext limited = context;
        limited.retry_after = *wait;
        refuse(c, "rate_limited", limited);
        return false;
    }
    ++c.in_flight;
    return true;
}

KeyDirectory::Client* KeyDirectory::settle(DirectoryClientId id) noexcept {
    Client* c = find(id);
    if (c != nullptr && c->in_flight > 0) {
        --c->in_flight;
    }
    return c;
}

void KeyDirectory::command(DirectoryClientId id, DirectoryCommand command) {
    Client* c = find(id);
    if (c == nullptr) {
        return;
    }
    if (auto* r = std::get_if<RegisterDevice>(&command)) {
        register_device(id, *c, *r);
    } else if (auto* t = std::get_if<RetireDevice>(&command)) {
        retire_device(id, *c, *t);
    } else if (auto* p = std::get_if<PublishKeyPackages>(&command)) {
        publish(id, *c, std::move(*p));
    } else if (auto* l = std::get_if<ListDevices>(&command)) {
        list_devices(id, *c, *l);
    } else {
        claim(id, *c, std::move(std::get<ClaimKeyPackages>(command)));
    }
}

void KeyDirectory::register_device(DirectoryClientId id, Client& asker,
                                   const RegisterDevice& command) {
    const DirectoryErrorContext context{
        .id = command.id, .device = command.device, .user = std::nullopt, .retry_after = {}};
    if (!admit(asker, true, context)) {
        return;
    }
    const std::weak_ptr<KeyDirectory*> self = self_;
    registry_->register_device(
        asker.user, command.device, [self, id, command, context](E2eeResult<void> result) noexcept {
            const auto alive = self.lock();
            if (!alive) {
                return;
            }
            KeyDirectory& d = **alive;
            Client* c = d.find(id);
            if (c == nullptr) {
                return;
            }
            if (!result) {
                static_cast<void>(d.settle(id));
                const std::string_view reason = reason_for(result.error(), true);
                if (reason == "device_limit") {
                    ++d.counters_.device_limit;
                    refuse(*c, reason, context);
                } else {
                    d.refuse_for(*c, result.error(), context);
                }
                return;
            }
            ++d.counters_.registered;
            try {
                if (std::ranges::find(c->devices, command.device) == c->devices.end()) {
                    c->devices.push_back(command.device);
                }
            } catch (const std::bad_alloc&) {
                c->client->allocation_failed();
            }
            d.answer_supply(id, "device_registered", command.device, command.id);
        });
}

void KeyDirectory::retire_device(DirectoryClientId id, Client& asker, const RetireDevice& command) {
    const DirectoryErrorContext context{
        .id = command.id, .device = command.device, .user = std::nullopt, .retry_after = {}};
    if (!admit(asker, true, context)) {
        return;
    }
    const std::weak_ptr<KeyDirectory*> self = self_;
    registry_->deregister_device(asker.user, command.device,
                                 [self, id, command, context](E2eeResult<void> result) noexcept {
                                     const auto alive = self.lock();
                                     if (!alive) {
                                         return;
                                     }
                                     KeyDirectory& d = **alive;
                                     Client* c = d.settle(id);
                                     if (c == nullptr) {
                                         return;
                                     }
                                     if (!result) {
                                         d.refuse_for(*c, result.error(), context);
                                         return;
                                     }
                                     ++d.counters_.retired;
                                     for (auto& [_, other] : d.clients_) {
                                         if (other.user == c->user) {
                                             std::erase(other.devices, command.device);
                                         }
                                     }
                                     try {
                                         std::string out;
                                         write_device_retired(out, command.device, command.id);
                                         push(*c, out);
                                     } catch (const std::bad_alloc&) {
                                         c->client->allocation_failed();
                                     }
                                 });
}

void KeyDirectory::publish(DirectoryClientId id, Client& asker, PublishKeyPackages command) {
    const DirectoryErrorContext context{
        .id = command.id, .device = command.device, .user = std::nullopt, .retry_after = {}};
    if (!admit(asker, true, context)) {
        return;
    }
    if (command.packages.empty()) {
        publish_last_resort(id, std::move(command));
        return;
    }
    const std::weak_ptr<KeyDirectory*> self = self_;
    const core::DeviceId device = command.device;
    std::vector<core::ports::KeyPackageBytes> batch = std::move(command.packages);
    command.packages.clear();
    const std::size_t count = batch.size();
    delivery_->publish_key_packages(asker.user, device, std::move(batch),
                                    [self, id, count, command = std::move(command),
                                     context](E2eeResult<std::size_t> result) mutable noexcept {
                                        const auto alive = self.lock();
                                        if (!alive) {
                                            return;
                                        }
                                        KeyDirectory& d = **alive;
                                        Client* c = d.find(id);
                                        if (c == nullptr) {
                                            return;
                                        }
                                        if (!result) {
                                            static_cast<void>(d.settle(id));
                                            d.refuse_for(*c, result.error(), context);
                                            return;
                                        }
                                        d.counters_.published += count;
                                        if (command.last_resort) {
                                            d.publish_last_resort(id, std::move(command));
                                            return;
                                        }
                                        d.answer_supply(id, "key_packages_published",
                                                        command.device, command.id);
                                    });
}

// The last-resort half of a publish, after its single-use packages if it had any; the command
// holds its place in flight already.
void KeyDirectory::publish_last_resort(DirectoryClientId id, PublishKeyPackages command) {
    Client* owner = find(id);
    if (owner == nullptr || !command.last_resort) {
        static_cast<void>(settle(id));
        return;
    }
    const DirectoryErrorContext context{
        .id = command.id, .device = command.device, .user = std::nullopt, .retry_after = {}};
    const std::weak_ptr<KeyDirectory*> self = self_;
    core::ports::KeyPackageBytes package = std::move(*command.last_resort);
    delivery_->publish_last_resort(owner->user, command.device, std::move(package),
                                   [self, id, device = command.device, request = command.id,
                                    context](E2eeResult<void> result) noexcept {
                                       const auto alive = self.lock();
                                       if (!alive) {
                                           return;
                                       }
                                       KeyDirectory& d = **alive;
                                       Client* c = d.find(id);
                                       if (c == nullptr) {
                                           return;
                                       }
                                       if (!result) {
                                           static_cast<void>(d.settle(id));
                                           d.refuse_for(*c, result.error(), context);
                                           return;
                                       }
                                       ++d.counters_.last_resorts_published;
                                       d.answer_supply(id, "key_packages_published", device,
                                                       request);
                                   });
}

void KeyDirectory::answer_supply(DirectoryClientId id, std::string_view type,
                                 const core::DeviceId& device,
                                 const std::optional<rt::MessageKey>& request) {
    Client* owner = find(id);
    if (owner == nullptr) {
        return;
    }
    const std::weak_ptr<KeyDirectory*> self = self_;
    registry_->list_devices(owner->user, [self, id, type, device, request](
                                             E2eeResult<std::vector<DeviceEntry>> result) noexcept {
        const auto alive = self.lock();
        if (!alive) {
            return;
        }
        KeyDirectory& d = **alive;
        Client* c = d.settle(id);
        if (c == nullptr) {
            return;
        }
        const DirectoryErrorContext context{
            .id = request, .device = device, .user = std::nullopt, .retry_after = {}};
        if (!result) {
            d.refuse_for(*c, result.error(), context);
            return;
        }
        const auto it = std::ranges::find(*result, device, &DeviceEntry::device);
        if (it == result->end()) {
            // Retired between the write and the read.
            d.refuse_for(*c, E2eeError::Revoked, context);
            return;
        }
        try {
            std::string out;
            write_device_supply(out, type, *it, request);
            push(*c, out);
        } catch (const std::bad_alloc&) {
            c->client->allocation_failed();
        }
    });
}

template <class Then>
void KeyDirectory::when_shared(DirectoryClientId id, Client& asker, const core::UserId& target,
                               const DirectoryErrorContext& context, Then then) {
    if (target == asker.user) {
        then();
        return;
    }
    const std::weak_ptr<KeyDirectory*> self = self_;
    access_.shared_with(
        asker.user, std::vector<core::UserId>{target},
        [self, id, target, context, then = std::move(then)](
            core::ports::MessageResult<std::vector<core::UserId>> result) mutable noexcept {
            const auto alive = self.lock();
            if (!alive) {
                return;
            }
            KeyDirectory& d = **alive;
            Client* c = d.find(id);
            if (c == nullptr) {
                return;
            }
            if (!result) {
                static_cast<void>(d.settle(id));
                ++d.counters_.unavailable;
                refuse(*c, "unavailable", context);
                return;
            }
            if (std::ranges::find(*result, target) == result->end()) {
                static_cast<void>(d.settle(id));
                ++d.counters_.not_shared;
                refuse(*c, "not_shared", context);
                return;
            }
            then();
        });
}

void KeyDirectory::list_devices(DirectoryClientId id, Client& asker, const ListDevices& command) {
    const DirectoryErrorContext context{
        .id = command.id, .device = std::nullopt, .user = command.user, .retry_after = {}};
    if (!admit(asker, false, context)) {
        return;
    }
    when_shared(id, asker, command.user, context, [this, id, command, context] {
        const std::weak_ptr<KeyDirectory*> self = self_;
        registry_->list_devices(
            command.user,
            [self, id, command, context](E2eeResult<std::vector<DeviceEntry>> result) noexcept {
                const auto alive = self.lock();
                if (!alive) {
                    return;
                }
                KeyDirectory& d = **alive;
                Client* c = d.settle(id);
                if (c == nullptr) {
                    return;
                }
                if (!result) {
                    d.refuse_for(*c, result.error(), context);
                    return;
                }
                ++d.counters_.listings;
                try {
                    std::string out;
                    // How many packages a device holds is its own user's business.
                    write_devices(out, command.user, *result, command.user == c->user, command.id);
                    push(*c, out);
                } catch (const std::bad_alloc&) {
                    c->client->allocation_failed();
                }
            });
    });
}

void KeyDirectory::claim(DirectoryClientId id, Client& asker, ClaimKeyPackages command) {
    const DirectoryErrorContext context{
        .id = command.id, .device = std::nullopt, .user = command.user, .retry_after = {}};
    if (!admit(asker, false, context)) {
        return;
    }
    auto pending = std::make_shared<Claim>(Claim{.client = id,
                                                 .user = command.user,
                                                 .id = command.id,
                                                 .devices = std::move(command.devices),
                                                 .pending = 0,
                                                 .outcome = {}});
    when_shared(id, asker, pending->user, context, [this, id, pending, context] {
        const std::weak_ptr<KeyDirectory*> self = self_;
        registry_->list_devices(
            pending->user,
            [self, id, pending, context](E2eeResult<std::vector<DeviceEntry>> result) noexcept {
                const auto alive = self.lock();
                if (!alive) {
                    return;
                }
                KeyDirectory& d = **alive;
                Client* c = d.find(id);
                if (c == nullptr) {
                    return;
                }
                if (!result) {
                    static_cast<void>(d.settle(id));
                    d.refuse_for(*c, result.error(), context);
                    return;
                }
                if (!choose_devices(*pending, *result)) {
                    static_cast<void>(d.settle(id));
                    c->client->allocation_failed();
                    return;
                }
                d.fetch_all(pending);
            });
    });
}

// The devices a claim takes a package of: those named that are live, or every live one. A
// device named but not live is gone, and costs nothing. False for want of memory.
bool KeyDirectory::choose_devices(Claim& claim, const std::vector<DeviceEntry>& live) noexcept {
    try {
        if (claim.devices.empty()) {
            for (const DeviceEntry& entry : live) {
                claim.devices.push_back(entry.device);
            }
            return true;
        }
        std::erase_if(claim.devices, [&](const core::DeviceId& device) {
            if (std::ranges::find(live, device, &DeviceEntry::device) != live.end()) {
                return false;
            }
            claim.outcome.gone.push_back(device);
            return true;
        });
    } catch (const std::bad_alloc&) {
        return false;
    }
    return true;
}

void KeyDirectory::fetch_all(const std::shared_ptr<Claim>& claim) {
    ++counters_.claims;
    claim->pending = claim->devices.size();
    if (claim->pending == 0) {
        finish_claim(*claim);
        return;
    }
    const std::weak_ptr<KeyDirectory*> self = self_;
    // A copy: answers may come back while this loop still runs, and never inside the call.
    const std::vector<core::DeviceId> devices = claim->devices;
    for (const core::DeviceId& device : devices) {
        delivery_->fetch_key_package(
            claim->user, device,
            [self, claim, device](E2eeResult<FetchedKeyPackage> result) mutable noexcept {
                if (const auto alive = self.lock()) {
                    (*alive)->fetched(claim, device, std::move(result));
                }
            });
    }
}

void KeyDirectory::fetched(const std::shared_ptr<Claim>& claim, const core::DeviceId& device,
                           E2eeResult<FetchedKeyPackage> result) noexcept {
    try {
        if (result) {
            ++counters_.claimed;
            if (result->last_resort) {
                ++counters_.claimed_last_resort;
            }
            if (result->replenish) {
                tell_replenish(device);
            }
            claim->outcome.claimed.push_back(ClaimedPackage{.device = device,
                                                            .package = std::move(result->package),
                                                            .last_resort = result->last_resort});
        } else {
            switch (result.error()) {
            case E2eeError::Exhausted:
                ++counters_.exhausted;
                tell_replenish(device);
                claim->outcome.exhausted.push_back(device);
                break;
            case E2eeError::NotFound:
            case E2eeError::Revoked:
                claim->outcome.gone.push_back(device);
                break;
            case E2eeError::Full:
            case E2eeError::Invalid:
            case E2eeError::StaleEpoch:
            case E2eeError::Unavailable:
            case E2eeError::Corrupt:
                claim->outcome.unavailable.push_back(device);
                break;
            }
        }
    } catch (const std::bad_alloc&) {
        // The package taken is lost with the answer; the client is closed for want of memory.
        if (Client* c = find(claim->client)) {
            c->client->allocation_failed();
        }
    }
    if (--claim->pending == 0) {
        finish_claim(*claim);
    }
}

void KeyDirectory::finish_claim(Claim& claim) noexcept {
    Client* c = settle(claim.client);
    if (c == nullptr) {
        // Taken for a client that left: those packages are spent, as for an invitation never
        // sent. Their devices publish more.
        return;
    }
    try {
        std::ranges::sort(claim.outcome.claimed, {}, &ClaimedPackage::device);
        std::ranges::sort(claim.outcome.exhausted);
        std::ranges::sort(claim.outcome.gone);
        std::ranges::sort(claim.outcome.unavailable);
        std::string out;
        write_claimed(out, claim.user, claim.outcome, claim.id);
        push(*c, out);
    } catch (const std::bad_alloc&) {
        c->client->allocation_failed();
    }
}

void KeyDirectory::tell_replenish(const core::DeviceId& device) noexcept {
    for (auto& [_, c] : clients_) {
        if (std::ranges::find(c.devices, device) == c.devices.end()) {
            continue;
        }
        try {
            std::string out;
            write_replenish(out, device);
            push(c, out);
            ++counters_.replenish_sent;
        } catch (const std::bad_alloc&) {
            c.client->allocation_failed();
        }
    }
}

} // namespace chat
