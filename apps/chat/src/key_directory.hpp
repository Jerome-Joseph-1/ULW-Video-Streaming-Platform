#pragma once

#include "core/models/ids.hpp"
#include "core/ports/clock.hpp"
#include "core/ports/e2ee.hpp"

#include "chat_service.hpp"
#include "envelope.hpp"
#include "presence.hpp"
#include "token_bucket.hpp"

#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include <string_view>
#include <unordered_map>
#include <vector>

namespace chat {

struct DirectoryLimits {
    // Listings and claims per user, across their connections on this node. Adding a group of 50
    // people claims for each of them once, and their devices are listed first by a client that
    // adds only the devices it is missing: 120 at once covers creating such a group, and 2 a
    // second after that is far more than people are added to conversations. Each claim reads
    // the user's devices and takes one package of each, at most kMaxDevicesPerUser statements.
    std::uint32_t read_burst = 120;
    std::uint32_t reads_per_second = 2;
    // Registrations, retirements and publishes per user. A device registers once per
    // connection and tops its supply up after it learns it is low: 20 at once, then one each
    // 3 s, keeps a script from rewriting its packages in a loop.
    std::uint32_t write_burst = 20;
    core::Millis write_interval{3'000};
    // Directory commands of one connection waiting for the database. A claim is one command
    // however many devices it reads.
    std::size_t max_in_flight = 8;
};

struct DirectoryCounters {
    std::uint64_t registered = 0;
    std::uint64_t retired = 0;
    // Single-use packages and last-resort packages stored by publishes.
    std::uint64_t published = 0;
    std::uint64_t last_resorts_published = 0;
    std::uint64_t listings = 0;
    std::uint64_t claims = 0;
    std::uint64_t claimed = 0;
    std::uint64_t claimed_last_resort = 0;
    // Devices a claim found with nothing left.
    std::uint64_t exhausted = 0;
    // `replenish` frames sent to this node's connections.
    std::uint64_t replenish_sent = 0;
    // Commands refused, by reason.
    std::uint64_t not_shared = 0;
    std::uint64_t rate_limited = 0;
    std::uint64_t busy = 0;
    std::uint64_t unknown_device = 0;
    std::uint64_t device_retired = 0;
    std::uint64_t device_limit = 0;
    std::uint64_t key_packages_full = 0;
    std::uint64_t unavailable = 0;
};

struct DirectoryClientId {
    std::uint64_t value = 0;
    friend bool operator==(DirectoryClientId, DirectoryClientId) = default;
};

// The key directory as clients see it (ADR-0102): each device of a user publishes its MLS
// KeyPackages, and whoever adds that user to a group claims one of each of their devices'. The
// directory itself (ADR-0038) holds the packages; this decides who may ask what, how often, and
// writes the answers. A device's packages are published only by its own user, which the
// directory checks against the device's row. Another user's devices are listed and claimed only
// by someone who shares a direct or group chat with that user, as presence is seen (ADR-0096).
// Packages are opaque here: carried, never read. Everything runs on the reactor thread.
class KeyDirectory {
public:
    // `registry` and `delivery` are null when no directory is configured: every command is then
    // answered unavailable. Answers that arrive after this is destroyed are dropped.
    KeyDirectory(core::ports::IDeviceRegistry* registry,
                 core::ports::IE2eeDeliveryService* delivery, IPresenceAccess& access,
                 const core::ports::IClock& clock, DirectoryLimits limits);
    ~KeyDirectory();
    KeyDirectory(const KeyDirectory&) = delete;
    KeyDirectory& operator=(const KeyDirectory&) = delete;
    KeyDirectory(KeyDirectory&&) = delete;
    KeyDirectory& operator=(KeyDirectory&&) = delete;

    [[nodiscard]] DirectoryClientId attach(IClient& client, const core::UserId& user);
    // Nothing reaches the client after this; its answers still owed are dropped.
    void detach(DirectoryClientId id) noexcept;
    void command(DirectoryClientId id, DirectoryCommand command);

    [[nodiscard]] const DirectoryCounters& counters() const noexcept { return counters_; }
    [[nodiscard]] std::size_t users() const noexcept { return users_.size(); }

private:
    struct Client {
        IClient* client;
        core::UserId user;
        std::size_t in_flight = 0;
        // Devices this connection registered: told when a claim leaves one low.
        std::vector<core::DeviceId> devices;
    };
    struct User {
        std::size_t clients = 0;
        TokenBucket reads;
        PacedBucket writes;
    };
    struct Claim;

    [[nodiscard]] Client* find(DirectoryClientId id) noexcept;
    [[nodiscard]] User& user_entry(const core::UserId& user);
    // Takes the allowance and a place in flight, or answers why not.
    [[nodiscard]] bool admit(Client& c, bool write, const DirectoryErrorContext& context);
    // The answer has come: the place in flight is free again. Null when the client is gone.
    [[nodiscard]] Client* settle(DirectoryClientId id) noexcept;
    static void refuse(Client& c, std::string_view reason,
                       const DirectoryErrorContext& context) noexcept;
    void refuse_for(Client& c, core::ports::E2eeError error,
                    const DirectoryErrorContext& context) noexcept;
    static void push(Client& c, std::string_view text) noexcept;

    void register_device(DirectoryClientId id, Client& asker, const RegisterDevice& command);
    void retire_device(DirectoryClientId id, Client& asker, const RetireDevice& command);
    void publish(DirectoryClientId id, Client& asker, PublishKeyPackages command);
    void publish_last_resort(DirectoryClientId id, PublishKeyPackages command);
    void list_devices(DirectoryClientId id, Client& asker, const ListDevices& command);
    void claim(DirectoryClientId id, Client& asker, ClaimKeyPackages command);
    // Runs `then` once `user` may read `target`'s devices: its own, or a user it shares a chat
    // with; answers not_shared or unavailable otherwise.
    template <class Then>
    void when_shared(DirectoryClientId id, Client& asker, const core::UserId& target,
                     const DirectoryErrorContext& context, Then then);
    // Answers with the device's supply as the directory lists it now.
    void answer_supply(DirectoryClientId id, std::string_view type, const core::DeviceId& device,
                       const std::optional<rt::MessageKey>& request);
    [[nodiscard]] static bool
    choose_devices(Claim& claim, const std::vector<core::ports::DeviceEntry>& live) noexcept;
    void fetch_all(const std::shared_ptr<Claim>& claim);
    // Every fetch has answered: the claim's answer goes to its client.
    void finish_claim(Claim& claim) noexcept;
    void fetched(const std::shared_ptr<Claim>& claim, const core::DeviceId& device,
                 core::ports::E2eeResult<core::ports::FetchedKeyPackage> result) noexcept;
    // A claim left `device` low: its registered connections here are told.
    void tell_replenish(const core::DeviceId& device) noexcept;
    void prune() noexcept;

    core::ports::IDeviceRegistry* registry_;
    core::ports::IE2eeDeliveryService* delivery_;
    IPresenceAccess& access_;
    const core::ports::IClock& clock_;
    DirectoryLimits limits_;
    DirectoryCounters counters_;
    std::uint64_t next_client_ = 1;
    std::unordered_map<std::uint64_t, Client> clients_;
    std::unordered_map<core::UserId, User> users_;
    // Callbacks hold it weakly: an answer that comes back after the service is gone is dropped.
    std::shared_ptr<KeyDirectory*> self_;
};

} // namespace chat
