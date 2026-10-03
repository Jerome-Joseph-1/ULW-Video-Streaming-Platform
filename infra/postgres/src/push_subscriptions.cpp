#include "infra/postgres/push_subscriptions.hpp"

#include "operation.hpp"
#include "pool.hpp"
#include "result.hpp"

#include <algorithm>
#include <limits>
#include <utility>

namespace infra::postgres {

namespace {

using core::ports::PushCallback;
using core::ports::PushResult;
using core::ports::PushStoreError;
using core::ports::PushSubscription;

// The most rows a list asks for, whatever its caller says: a user's devices.
constexpr std::size_t kMaxList = 64;

constexpr Sql kSave = "SELECT push_subscribe($1, $2, $3, $4, $5, $6::integer)";
constexpr Sql kRemove = "DELETE FROM push_subscriptions WHERE user_id = $1 AND device_id = $2";
constexpr Sql kList = "SELECT device_id, endpoint, p256dh, auth FROM push_subscriptions "
                      "WHERE user_id = $1 ORDER BY updated_at DESC, device_id DESC LIMIT $2";
constexpr Sql kForget = "DELETE FROM push_subscriptions WHERE endpoint = $1";

template <std::size_t N> std::span<const std::byte> bytes(const std::array<std::uint8_t, N>& a) {
    return std::as_bytes(std::span(a));
}

// One statement whose only answer is whether it ran. Owns everything it binds.
class Write final : public Operation {
public:
    Write(Sql sql, std::string text, std::optional<core::DeviceId> device,
          std::optional<PushSubscription> subscription, std::int64_t max, PushCallback<void> done)
        : sql_(sql), text_(std::move(text)), device_(device),
          subscription_(std::move(subscription)), max_(max), done_(std::move(done)) {}

    [[nodiscard]] Statement start() noexcept override {
        Params params;
        params.add_text(text_);
        if (subscription_) {
            params.add_uuid(subscription_->device.uuid())
                .add_text(subscription_->endpoint)
                .add_bytea(bytes(subscription_->p256dh))
                .add_bytea(bytes(subscription_->auth))
                .add_int(max_);
        } else if (device_) {
            params.add_uuid(device_->uuid());
        }
        return Statement{.sql = sql_, .params = params};
    }

    [[nodiscard]] std::optional<Statement> next(Outcome outcome) noexcept override {
        if (!outcome) {
            done_(std::unexpected(PushStoreError::Unavailable));
        } else {
            done_({});
        }
        return std::nullopt;
    }

    void abandon(DbError /*error*/) noexcept override {
        done_(std::unexpected(PushStoreError::Unavailable));
    }

private:
    Sql sql_;
    // The user, or the endpoint for a forget.
    std::string text_;
    std::optional<core::DeviceId> device_;
    std::optional<PushSubscription> subscription_;
    std::int64_t max_;
    PushCallback<void> done_;
};

template <std::size_t N>
bool copy_exact(const std::optional<std::vector<std::byte>>& from,
                std::array<std::uint8_t, N>& to) {
    if (!from || from->size() != N) {
        return false;
    }
    std::ranges::transform(*from, to.begin(),
                           [](std::byte b) { return std::to_integer<std::uint8_t>(b); });
    return true;
}

PushResult<std::vector<PushSubscription>> decode_list(const Result& r) {
    std::vector<PushSubscription> out;
    out.reserve(static_cast<std::size_t>(r.rows()));
    for (int row = 0; row < r.rows(); ++row) {
        const auto device = domain_at<core::DeviceId>(r, row, 0);
        const auto endpoint = r.get(row, 1);
        if (!device || !endpoint) {
            return std::unexpected(PushStoreError::Corrupt);
        }
        PushSubscription s{
            .device = *device, .endpoint = std::string(*endpoint), .p256dh = {}, .auth = {}};
        if (!copy_exact(r.get(row, 2).and_then(parse_bytea), s.p256dh) ||
            !copy_exact(r.get(row, 3).and_then(parse_bytea), s.auth)) {
            return std::unexpected(PushStoreError::Corrupt);
        }
        out.push_back(std::move(s));
    }
    return out;
}

class List final : public Operation {
public:
    List(const core::UserId& user, std::size_t limit,
         PushCallback<std::vector<PushSubscription>> done)
        : user_(user), limit_(std::min(limit, kMaxList)), done_(std::move(done)) {}

    [[nodiscard]] Statement start() noexcept override {
        return Statement{
            .sql = kList,
            .params = Params{}.add_text(user_.view()).add_int(static_cast<std::int64_t>(limit_))};
    }

    [[nodiscard]] std::optional<Statement> next(Outcome outcome) noexcept override {
        if (!outcome) {
            done_(std::unexpected(PushStoreError::Unavailable));
            return std::nullopt;
        }
        try {
            done_(decode_list(*outcome));
        } catch (const std::bad_alloc&) {
            done_(std::unexpected(PushStoreError::Unavailable));
        }
        return std::nullopt;
    }

    void abandon(DbError /*error*/) noexcept override {
        done_(std::unexpected(PushStoreError::Unavailable));
    }

private:
    core::UserId user_;
    std::size_t limit_;
    PushCallback<std::vector<PushSubscription>> done_;
};

} // namespace

class PgPushSubscriptions::Impl {
public:
    explicit Impl(std::unique_ptr<Pool> pool) noexcept : pool_(std::move(pool)) {}
    [[nodiscard]] Pool& pool() noexcept { return *pool_; }

private:
    std::unique_ptr<Pool> pool_;
};

std::expected<std::unique_ptr<PgPushSubscriptions>, std::string>
PgPushSubscriptions::create(net::IReactor& reactor, net::OffloadPool& offload,
                            const PushSubscriptionsConfig& config) {
    auto pool = Pool::create(reactor, offload,
                             PoolConfig{.conninfo = config.conninfo,
                                        .application_name = "ulw-push",
                                        .connections = config.connections,
                                        .connect_timeout = config.connect_timeout,
                                        .request_timeout = config.request_timeout});
    if (!pool) {
        return std::unexpected(std::move(pool.error()));
    }
    return std::make_unique<PgPushSubscriptions>(Token{}, std::make_unique<Impl>(std::move(*pool)));
}

PgPushSubscriptions::PgPushSubscriptions(Token /*token*/, std::unique_ptr<Impl> impl) noexcept
    : impl_(std::move(impl)) {}

PgPushSubscriptions::~PgPushSubscriptions() = default;

void PgPushSubscriptions::save(const core::UserId& user, const PushSubscription& subscription,
                               std::size_t max_per_user, PushCallback<void> done) {
    const auto max = static_cast<std::int64_t>(
        std::min<std::size_t>(max_per_user, std::numeric_limits<std::int32_t>::max()));
    impl_->pool().submit(std::make_unique<Write>(kSave, std::string(user.view()), std::nullopt,
                                                 subscription, max, std::move(done)));
}

void PgPushSubscriptions::remove(const core::UserId& user, const core::DeviceId& device,
                                 PushCallback<void> done) {
    impl_->pool().submit(std::make_unique<Write>(kRemove, std::string(user.view()), device,
                                                 std::nullopt, 0, std::move(done)));
}

void PgPushSubscriptions::list(const core::UserId& user, std::size_t limit,
                               PushCallback<std::vector<PushSubscription>> done) {
    impl_->pool().submit(std::make_unique<List>(user, limit, std::move(done)));
}

void PgPushSubscriptions::forget(const std::string& endpoint, PushCallback<void> done) {
    impl_->pool().submit(
        std::make_unique<Write>(kForget, endpoint, std::nullopt, std::nullopt, 0, std::move(done)));
}

} // namespace infra::postgres
