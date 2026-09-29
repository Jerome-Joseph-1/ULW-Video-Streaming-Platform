#pragma once

#include "infra/postgres/e2ee_directory.hpp"
#include "net/offload_pool.hpp"
#include "net/reactor_factory.hpp"

#include "e2ee_directory_contract.hpp"
#include "postgres_harness.hpp"
#include "support/fake_clock.hpp"

#include <cstddef>
#include <gtest/gtest.h>
#include <memory>
#include <utility>

namespace ulw::test {

// The Postgres directory on a scratch database of its own, with the loop that drives it.
class PgDirectoryHarness final : public DirectoryHarness {
public:
    static std::unique_ptr<PgDirectoryHarness> open(std::size_t connections) {
        auto harness = std::make_unique<PgDirectoryHarness>();
        ScratchDatabase::open(harness->db_);
        if (harness->db_ == nullptr) {
            return nullptr;
        }
        auto r = net::make_reactor_with_fallback(net::ReactorKind::IoUring, harness->clock_, 4096);
        if (!r) {
            ADD_FAILURE() << "reactor: " << r.error();
            return nullptr;
        }
        harness->reactor_ = std::move(r->reactor);
        auto offload = net::OffloadPool::create(*harness->reactor_, 1);
        if (!offload) {
            ADD_FAILURE() << "offload pool";
            return nullptr;
        }
        harness->offload_ = std::move(*offload);
        // Long enough that a statement queued behind a test's gate is never cancelled.
        auto made = infra::postgres::PgE2eeDirectory::create(
            *harness->reactor_, *harness->offload_,
            infra::postgres::E2eeDirectoryConfig{.conninfo = harness->db_->conninfo(),
                                                 .connections = connections,
                                                 .request_timeout = core::Millis{30000}});
        if (!made) {
            ADD_FAILURE() << made.error();
            return nullptr;
        }
        harness->directory_ = std::move(*made);
        return harness;
    }

    PgDirectoryHarness() = default;
    ~PgDirectoryHarness() override {
        // The directory requires the offload pool stopped before it goes.
        offload_.reset();
        directory_.reset();
        reactor_.reset();
    }
    PgDirectoryHarness(const PgDirectoryHarness&) = delete;
    PgDirectoryHarness& operator=(const PgDirectoryHarness&) = delete;
    PgDirectoryHarness(PgDirectoryHarness&&) = delete;
    PgDirectoryHarness& operator=(PgDirectoryHarness&&) = delete;

    net::IReactor& reactor() override { return *reactor_; }
    core::ports::IDeviceRegistry& registry() override { return *directory_; }
    core::ports::IE2eeDeliveryService& delivery() override { return *directory_; }
    ScratchDatabase& db() { return *db_; }

private:
    ulw::test::FakeClock clock_;
    std::unique_ptr<ScratchDatabase> db_;
    std::unique_ptr<net::IReactor> reactor_;
    std::unique_ptr<net::OffloadPool> offload_;
    std::unique_ptr<infra::postgres::PgE2eeDirectory> directory_;
};

} // namespace ulw::test
