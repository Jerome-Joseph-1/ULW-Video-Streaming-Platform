#pragma once

#include "infra/catalog/memory_catalog.hpp"
#include "infra/storage/fake_store.hpp"

#include "gateway.hpp"

#include <functional>
#include <future>
#include <memory>
#include <thread>

namespace ulw::test {

enum class Backend { Fake, Fs };

struct GatewayOptions {
    Backend backend = Backend::Fs;
    std::uint64_t chunk = std::uint64_t{8} * 1024 * 1024;
    infra::storage::FaultPlan plan{};
    gateway::Limits limits{};
};

// A gateway shard on its own reactor thread, as in production, reachable over loopback.
// Everything the gateway owns is created, used and destroyed on that thread.
class GatewayUnderTest {
public:
    explicit GatewayUnderTest(GatewayOptions options);
    ~GatewayUnderTest();
    GatewayUnderTest(const GatewayUnderTest&) = delete;
    GatewayUnderTest& operator=(const GatewayUnderTest&) = delete;

    [[nodiscard]] std::uint16_t port() const { return port_; }
    // Thread-safe reads of committed objects.
    [[nodiscard]] core::ports::IObjectReader& reader() { return *reader_; }
    void set_plan(const infra::storage::FaultPlan& plan);

    // Runs `fn` on the loop thread and waits for it.
    void on_loop(std::function<void()> fn);
    [[nodiscard]] std::vector<infra::catalog::MemoryCatalog::Job> jobs();
    [[nodiscard]] gateway::Counters counters();
    [[nodiscard]] std::size_t connections();
    [[nodiscard]] std::size_t claims();
    void drain();

private:
    struct Loop;
    void run(const GatewayOptions& options, std::promise<void> ready);

    std::unique_ptr<Loop> loop_;
    std::uint16_t port_ = 0;
    core::ports::IObjectReader* reader_ = nullptr;
    infra::storage::FakeStore* fake_ = nullptr;
    std::jthread thread_;
};

} // namespace ulw::test
