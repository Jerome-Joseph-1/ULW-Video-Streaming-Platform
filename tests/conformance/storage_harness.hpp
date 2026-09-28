#pragma once

#include "core/ports/storage.hpp"
#include "infra/storage/fake_store.hpp"
#include "net/reactor.hpp"

#include "support/reactor_harness.hpp"

#include <chrono>
#include <functional>
#include <memory>
#include <ostream>
#include <span>
#include <string>
#include <vector>

namespace ulw::test {

struct Observer final : core::ports::IIngestObserver {
    int calls = 0;
    std::function<void()> on_progress;
    void on_ingest_progress() noexcept override {
        ++calls;
        if (on_progress) {
            on_progress();
        }
    }
};

// One backend plus the loop that drives it.
class StorageHarness {
public:
    StorageHarness() = default;
    StorageHarness(const StorageHarness&) = delete;
    StorageHarness& operator=(const StorageHarness&) = delete;
    virtual ~StorageHarness() = default;

    virtual core::ports::IIngestStore& ingest() = 0;
    virtual core::ports::IObjectReader& reader() = 0;
    virtual core::ports::IObjectAdmin& admin() = 0;
    // Keeps one run's keys apart from another's on shared live buckets.
    [[nodiscard]] virtual std::string key_prefix() const { return {}; }
    // How long a live backend may stay silent while still finishing work it accepted.
    [[nodiscard]] virtual std::chrono::milliseconds quiet_period() const {
        return std::chrono::milliseconds(50);
    }

    [[nodiscard]] net::IReactor& reactor() { return *reactor_; }

    // Writes every byte, turning the loop whenever the store pushes back.
    [[nodiscard]] bool write_all(core::ports::IIngestSession& session, Observer& observer,
                                 std::span<const std::byte> bytes);
    [[nodiscard]] bool finish(core::ports::IIngestSession& session, Observer& observer);
    // Turns the loop until the backend has finished every piece of work it accepted. A live
    // bucket gives no signal for that, so the default waits for quiet_period() of silence;
    // local backends override it with one that does not depend on timing.
    virtual void settle(Observer& observer);

protected:
    std::unique_ptr<net::IReactor> reactor_;
};

struct StoreFactory {
    std::string name;
    std::function<std::unique_ptr<StorageHarness>()> make;
    friend void PrintTo(const StoreFactory& f, std::ostream* os) { *os << f.name; }
};

// A bound on any one wait, far above what a live bucket needs, so a hung backend fails the
// test instead of the whole run.
inline constexpr auto kOperationLimit = std::chrono::seconds(30);

// Chunk size the local backends are built with: small enough to keep the suite fast, large
// enough that every law spans several chunks.
inline constexpr std::uint64_t kLocalChunk = std::uint64_t{1024} * 1024;

[[nodiscard]] std::vector<StoreFactory> store_factories();

class FakeHarness final : public StorageHarness {
public:
    FakeHarness();
    ~FakeHarness() override;
    core::ports::IIngestStore& ingest() override;
    core::ports::IObjectReader& reader() override;
    core::ports::IObjectAdmin& admin() override;
    void settle(Observer& observer) override;
    [[nodiscard]] infra::storage::FakeStore& fake() { return *store_; }

private:
    std::unique_ptr<infra::storage::FakeStore> store_;
};
[[nodiscard]] net::ReactorKind reactor_kind_from_env();

// Creates, writes and finishes a whole object; the caller commits.
[[nodiscard]] core::ports::IngestId upload_whole(StorageHarness& h, const core::StorageKey& key,
                                                 std::span<const std::byte> data);

} // namespace ulw::test
