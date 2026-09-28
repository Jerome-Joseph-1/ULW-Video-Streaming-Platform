#include "core/ports/auth.hpp"
#include "core/util/time.hpp"
#include "infra/auth/base64url.hpp"
#include "infra/auth/jwks_verifier.hpp"
#include "net/reactor.hpp"
#include "net/reactor_factory.hpp"

#include "support/fake_clock.hpp"
#include "test_claims.hpp"
#include "test_keys.hpp"

#include <array>
#include <chrono>
#include <expected>
#include <gtest/gtest.h>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace {

using core::ports::AuthError;
using core::ports::VerifyResult;
using infra::auth::encode_base64url;
using infra::auth::IKeySetFetcher;
using infra::auth::IKeySetReceiver;
using infra::auth::JwksConfig;
using infra::auth::JwksVerifier;
using std::chrono::milliseconds;
using std::chrono::minutes;
using std::chrono::seconds;
using ulw::test::key_set;
using ulw::test::signed_token;
using ulw::test::test_payload;
using ulw::test::TestKey;

constexpr std::string_view kUrl = "https://id.askedin.test/.well-known/jwks.json";

// Key generation dominates this suite's run time and ctest runs every test in a process of
// its own, so each key is made once per process and only when a test asks for it.
const TestKey& rsa_key() {
    static const TestKey key = TestKey::rsa("rsa-1");
    return key;
}

const TestKey& ec_key() {
    static const TestKey key = TestKey::p256("ec-1");
    return key;
}

const TestKey& ed_key() {
    static const TestKey key = TestKey::ed25519("ed-1");
    return key;
}

const TestKey& next_ed_key() {
    static const TestKey key = TestKey::ed25519("ed-2");
    return key;
}

const TestKey& small_rsa_key() {
    static const TestKey key = TestKey::rsa("rsa-small", 1024);
    return key;
}

// Hands out one fetch at a time and completes it when the test says so.
class FakeFetcher final : public IKeySetFetcher {
public:
    void fetch(std::string_view url, IKeySetReceiver& receiver) noexcept override {
        ++requests;
        urls.emplace_back(url);
        receiver_ = &receiver;
        if (answer_inside_fetch) {
            respond(*answer_inside_fetch);
        }
    }

    void cancel(IKeySetReceiver& receiver) noexcept override {
        if (receiver_ == &receiver) {
            receiver_ = nullptr;
            ++cancels;
        }
    }

    void respond(std::optional<std::string_view> body) {
        IKeySetReceiver* receiver = std::exchange(receiver_, nullptr);
        ASSERT_NE(receiver, nullptr) << "no fetch in flight";
        receiver->on_key_set(body);
    }

    [[nodiscard]] bool in_flight() const noexcept { return receiver_ != nullptr; }

    int requests = 0;
    int cancels = 0;
    std::vector<std::string> urls;
    std::optional<std::string> answer_inside_fetch;

private:
    IKeySetReceiver* receiver_ = nullptr;
};

struct CountingWaiter : core::ports::IKeyWaiter {
    int calls = 0;
    void on_keys_refreshed() noexcept override { ++calls; }
};

// Cancels another waiter from inside its own notification.
struct CancellingWaiter final : CountingWaiter {
    core::ports::IJwtVerifier* verifier = nullptr;
    core::ports::IKeyWaiter* victim = nullptr;
    void on_keys_refreshed() noexcept override {
        CountingWaiter::on_keys_refreshed();
        verifier->cancel_wait(*victim);
    }
};

// An EdDSA token under any kid; only the kid matters to the tests that use it.
std::string token_with_kid(std::string_view kid) {
    const std::string header = R"({"alg":"EdDSA","kid":")" + std::string(kid) + R"("})";
    const std::string input = encode_base64url(header) + '.' + encode_base64url(test_payload());
    return input + '.' + encode_base64url(ed_key().sign("EdDSA", input));
}

std::unexpected<AuthError> refused(AuthError e) {
    return std::unexpected(e);
}

class JwksVerifierTest : public ::testing::Test {
protected:
    void SetUp() override {
        auto reactor = net::make_reactor(net::ReactorKind::Epoll, clock_, 64);
        ASSERT_TRUE(reactor.has_value());
        reactor_ = std::move(*reactor);
        verifier_ = std::make_unique<JwksVerifier>(
            *reactor_, fetcher_,
            JwksConfig{.url = std::string(kUrl), .claims = ulw::test::kTestRules});
    }

    // Moves both clocks on and runs one loop iteration, which fires whatever timers are due.
    void advance(core::Millis d) {
        clock_.advance(d);
        pump();
    }
    void pump() { reactor_->run_once(core::Millis{0}); }

    std::optional<VerifyResult> verify(const std::string& token, core::ports::IKeyWaiter& waiter) {
        return verifier_->verify(token, clock_.wall_now(), waiter);
    }
    std::optional<VerifyResult> verify(const std::string& token) { return verify(token, waiter_); }

    // A token whose kid is unseen: it waits, `jwks` answers the fetch, the waiter hears back
    // on the next iteration, and verifying again answers.
    VerifyResult verify_through_fetch(const std::string& token, std::string_view jwks) {
        CountingWaiter waiter;
        EXPECT_FALSE(verify(token, waiter).has_value()) << "answered without a fetch";
        fetcher_.respond(jwks);
        pump();
        EXPECT_EQ(waiter.calls, 1);
        return verify(token, waiter).value_or(refused(AuthError::KeysUnavailable));
    }

    ulw::test::FakeClock clock_;
    std::unique_ptr<net::IReactor> reactor_;
    FakeFetcher fetcher_;
    CountingWaiter waiter_;
    std::unique_ptr<JwksVerifier> verifier_;
};

TEST_F(JwksVerifierTest, ATokenForTheSecondKeyVerifiesAfterItsOneFetch) {
    const std::string for_second = signed_token(ec_key(), "ES256", test_payload());
    CountingWaiter waiter;
    EXPECT_FALSE(verify(for_second, waiter).has_value());
    EXPECT_EQ(fetcher_.requests, 1);
    EXPECT_EQ(fetcher_.urls.back(), kUrl);

    fetcher_.respond(key_set({rsa_key().jwk(), ec_key().jwk()}));
    EXPECT_EQ(waiter.calls, 0) << "called from inside the fetch callback";
    pump();
    EXPECT_EQ(waiter.calls, 1);

    const std::optional<VerifyResult> verified = verify(for_second, waiter);
    ASSERT_TRUE(verified.has_value());
    ASSERT_TRUE(verified->has_value());
    EXPECT_EQ((*verified)->subject.view(), "alice");
    // The first key came with the same fetch.
    EXPECT_TRUE(verify(signed_token(rsa_key(), "RS256", test_payload()))
                    .value_or(refused(AuthError::KeysUnavailable)));
    EXPECT_EQ(fetcher_.requests, 1);
    pump();
    EXPECT_EQ(waiter.calls, 1);
}

TEST_F(JwksVerifierTest, EveryAlgorithmVerifiesAgainstThePublishedKeys) {
    const std::string jwks = key_set({rsa_key().jwk(), ec_key().jwk(), ed_key().jwk()});
    EXPECT_TRUE(verify_through_fetch(signed_token(rsa_key(), "RS256", test_payload()), jwks));
    for (const auto& [key, alg] : {std::pair{&rsa_key(), "PS256"}, std::pair{&ec_key(), "ES256"},
                                   std::pair{&ed_key(), "EdDSA"}}) {
        const std::optional<VerifyResult> result = verify(signed_token(*key, alg, test_payload()));
        ASSERT_TRUE(result.has_value()) << alg;
        EXPECT_TRUE(result->has_value()) << alg;
    }
    EXPECT_EQ(fetcher_.requests, 1);
}

TEST_F(JwksVerifierTest, MissesDuringAFetchShareItAndEachWaiterHearsBackOnce) {
    std::array<CountingWaiter, 3> waiters{};
    EXPECT_FALSE(verify(signed_token(rsa_key(), "RS256", test_payload()), waiters[0]));
    EXPECT_FALSE(verify(signed_token(ec_key(), "ES256", test_payload()), waiters[1]));
    EXPECT_FALSE(verify(token_with_kid("never-published"), waiters[2]));
    // The same waiter asking twice is still one waiter.
    EXPECT_FALSE(verify(token_with_kid("never-published"), waiters[2]));
    EXPECT_EQ(fetcher_.requests, 1);

    fetcher_.respond(key_set({rsa_key().jwk(), ec_key().jwk()}));
    pump();
    pump();
    for (const CountingWaiter& w : waiters) {
        EXPECT_EQ(w.calls, 1);
    }
    EXPECT_EQ(fetcher_.requests, 1);
}

TEST_F(JwksVerifierTest, ACancelledWaiterIsNotCalled) {
    CountingWaiter stays;
    CountingWaiter leaves;
    EXPECT_FALSE(verify(signed_token(ed_key(), "EdDSA", test_payload()), stays));
    EXPECT_FALSE(verify(signed_token(ed_key(), "EdDSA", test_payload()), leaves));
    verifier_->cancel_wait(leaves);
    fetcher_.respond(key_set({ed_key().jwk()}));
    pump();
    EXPECT_EQ(stays.calls, 1);
    EXPECT_EQ(leaves.calls, 0);
}

TEST_F(JwksVerifierTest, AWaiterCancelledByAnEarlierWaitersCallbackIsNotCalled) {
    CountingWaiter victim;
    CancellingWaiter first;
    first.verifier = verifier_.get();
    first.victim = &victim;
    EXPECT_FALSE(verify(signed_token(ed_key(), "EdDSA", test_payload()), first));
    EXPECT_FALSE(verify(signed_token(ed_key(), "EdDSA", test_payload()), victim));
    fetcher_.respond(key_set({ed_key().jwk()}));
    pump();
    EXPECT_EQ(first.calls, 1);
    EXPECT_EQ(victim.calls, 0);
}

TEST_F(JwksVerifierTest, AnUnknownKidCostsOneFetchThenIsRefusedForAMinute) {
    const std::string jwks = key_set({ed_key().jwk()});
    EXPECT_TRUE(verify_through_fetch(signed_token(ed_key(), "EdDSA", test_payload()), jwks));
    advance(seconds(11));

    const std::string stranger = token_with_kid("stranger");
    EXPECT_EQ(verify_through_fetch(stranger, jwks), refused(AuthError::UnknownKey));
    EXPECT_EQ(fetcher_.requests, 2);

    advance(seconds(59));
    EXPECT_EQ(verify(stranger), refused(AuthError::UnknownKey));
    EXPECT_EQ(fetcher_.requests, 2);

    advance(seconds(1));
    EXPECT_FALSE(verify(stranger).has_value());
    EXPECT_EQ(fetcher_.requests, 3);
}

TEST_F(JwksVerifierTest, DistinctJunkKidsCannotForceBackToBackFetches) {
    EXPECT_TRUE(verify_through_fetch(signed_token(ed_key(), "EdDSA", test_payload()),
                                     key_set({ed_key().jwk()})));
    for (int i = 0; i < 20; ++i) {
        EXPECT_EQ(verify(token_with_kid("junk-" + std::to_string(i))),
                  refused(AuthError::UnknownKey));
        clock_.advance(milliseconds(450));
        pump();
    }
    EXPECT_EQ(fetcher_.requests, 1);
    advance(milliseconds(1100));
    EXPECT_FALSE(verify(token_with_kid("junk-late")).has_value());
    EXPECT_EQ(fetcher_.requests, 2);
}

TEST_F(JwksVerifierTest, TokensThatCanNeverVerifyCauseNoFetch) {
    const std::string none =
        ulw::test::compact(R"({"alg":"none","kid":"rsa-1"})", test_payload(), "");
    EXPECT_EQ(verify(none), refused(AuthError::UnsupportedAlgorithm));

    const std::string header = R"({"alg":"HS256","kid":"rsa-1"})";
    const std::string input = encode_base64url(header) + '.' + encode_base64url(test_payload());
    const std::string hs256 =
        input + '.' + encode_base64url(ulw::test::hmac_sha256(rsa_key().public_pem(), input));
    EXPECT_EQ(verify(hs256), refused(AuthError::UnsupportedAlgorithm));

    EXPECT_EQ(verify("a.b"), refused(AuthError::Malformed));
    EXPECT_EQ(verify(std::string(9000, 'a')), refused(AuthError::Malformed));
    EXPECT_EQ(fetcher_.requests, 0);
}

TEST_F(JwksVerifierTest, ARotatedKeyIsPickedUpWithoutARestart) {
    const std::string old_token = signed_token(ed_key(), "EdDSA", test_payload());
    EXPECT_TRUE(verify_through_fetch(old_token, key_set({ed_key().jwk()})));
    advance(seconds(11));

    const std::string new_token = signed_token(next_ed_key(), "EdDSA", test_payload());
    EXPECT_TRUE(verify_through_fetch(new_token, key_set({next_ed_key().jwk()})));
    EXPECT_EQ(fetcher_.requests, 2);
    // The old key left the set with that fetch, and the verdict it had produced with it.
    EXPECT_EQ(verify(old_token), refused(AuthError::UnknownKey));
    EXPECT_EQ(fetcher_.requests, 2);
}

TEST_F(JwksVerifierTest, KeysAreRefetchedEvery15MinutesAndServeUntilTheRefetchLands) {
    EXPECT_TRUE(verify_through_fetch(signed_token(ed_key(), "EdDSA", test_payload()),
                                     key_set({ed_key().jwk()})));
    advance(minutes(15) - seconds(1));
    EXPECT_EQ(fetcher_.requests, 1);
    advance(milliseconds(1200));
    EXPECT_EQ(fetcher_.requests, 2);
    ASSERT_TRUE(fetcher_.in_flight());

    // Stale, and still answering at once.
    const std::string bob = signed_token(ed_key(), "EdDSA", test_payload({{"sub", R"("bob")"}}));
    const std::optional<VerifyResult> during = verify(bob);
    ASSERT_TRUE(during.has_value());
    EXPECT_TRUE(during->has_value());

    fetcher_.respond(key_set({next_ed_key().jwk()}));
    EXPECT_EQ(verify(bob), refused(AuthError::UnknownKey));
    advance(minutes(15) + milliseconds(200));
    EXPECT_EQ(fetcher_.requests, 3);
}

TEST_F(JwksVerifierTest, AFailedRefetchKeepsTheKeysAndRetriesWithCappedBackoff) {
    EXPECT_TRUE(verify_through_fetch(signed_token(ed_key(), "EdDSA", test_payload()),
                                     key_set({ed_key().jwk()})));
    advance(minutes(15) + milliseconds(200));
    ASSERT_EQ(fetcher_.requests, 2);

    int expected_requests = 2;
    for (const int delay : {1, 2, 4, 8, 16, 32, 60, 60}) {
        fetcher_.respond(std::nullopt);
        const std::string fresh = signed_token(
            ed_key(), "EdDSA", test_payload({{"sub", '"' + std::to_string(delay) + '"'}}));
        const std::optional<VerifyResult> kept = verify(fresh);
        ASSERT_TRUE(kept.has_value());
        EXPECT_TRUE(kept->has_value()) << "old keys dropped after a failed fetch";
        EXPECT_EQ(verify(token_with_kid("brand-new")), refused(AuthError::KeysUnavailable));

        advance(seconds(delay) - milliseconds(200));
        EXPECT_EQ(fetcher_.requests, expected_requests) << "retried early, delay " << delay;
        advance(milliseconds(400));
        EXPECT_EQ(fetcher_.requests, ++expected_requests) << "no retry, delay " << delay;
    }

    fetcher_.respond(key_set({ed_key().jwk()}));
    advance(minutes(14));
    EXPECT_EQ(fetcher_.requests, expected_requests);
    advance(minutes(1) + milliseconds(200));
    EXPECT_EQ(fetcher_.requests, expected_requests + 1);
}

TEST_F(JwksVerifierTest, AFailedFirstFetchAnswersKeysUnavailableUntilARetryLands) {
    const std::string token = signed_token(ed_key(), "EdDSA", test_payload());
    EXPECT_EQ(verify_through_fetch(token, ""), refused(AuthError::KeysUnavailable));
    EXPECT_EQ(fetcher_.requests, 1);

    advance(milliseconds(1200));
    EXPECT_EQ(fetcher_.requests, 2);
    fetcher_.respond(key_set({ed_key().jwk()}));
    pump();
    EXPECT_EQ(waiter_.calls, 0);
    const std::optional<VerifyResult> result = verify(token);
    ASSERT_TRUE(result.has_value());
    EXPECT_TRUE(result->has_value());
}

TEST_F(JwksVerifierTest, ASetWithoutAUsableKeyCountsAsAFailedFetch) {
    const std::string token = signed_token(ed_key(), "EdDSA", test_payload());
    EXPECT_EQ(verify_through_fetch(token, R"({"keys":[]})"), refused(AuthError::KeysUnavailable));

    advance(milliseconds(1200));
    fetcher_.respond(key_set({ed_key().jwk()}));
    EXPECT_TRUE(verify(token).value_or(refused(AuthError::KeysUnavailable)).has_value());

    advance(minutes(15) + milliseconds(200));
    fetcher_.respond(key_set({small_rsa_key().jwk()}));
    const std::string bob = signed_token(ed_key(), "EdDSA", test_payload({{"sub", R"("bob")"}}));
    EXPECT_TRUE(verify(bob).value_or(refused(AuthError::KeysUnavailable)).has_value());
    const int before = fetcher_.requests;
    advance(milliseconds(1200));
    EXPECT_EQ(fetcher_.requests, before + 1);
}

TEST_F(JwksVerifierTest, AFetchCompletedInsideFetchStillCallsTheWaiterFromTheLoop) {
    fetcher_.answer_inside_fetch = key_set({ed_key().jwk()});
    const std::string token = signed_token(ed_key(), "EdDSA", test_payload());
    CountingWaiter waiter;
    EXPECT_FALSE(verify(token, waiter).has_value());
    EXPECT_EQ(waiter.calls, 0);
    pump();
    EXPECT_EQ(waiter.calls, 1);
    EXPECT_TRUE(verify(token, waiter).value_or(refused(AuthError::KeysUnavailable)).has_value());
}

TEST_F(JwksVerifierTest, AVerdictIsNotReusedPastTheTokensExpiry) {
    const std::string short_lived =
        signed_token(ed_key(), "EdDSA", test_payload({{"exp", ulw::test::numeric_date(120)}}));
    EXPECT_TRUE(verify_through_fetch(short_lived, key_set({ed_key().jwk()})));
    advance(seconds(179));
    EXPECT_TRUE(verify(short_lived).value_or(refused(AuthError::KeysUnavailable)).has_value());
    advance(seconds(1));
    EXPECT_EQ(verify(short_lived), refused(AuthError::Expired));
}

TEST_F(JwksVerifierTest, DestructionCancelsTheFetchInFlight) {
    EXPECT_FALSE(verify(signed_token(ed_key(), "EdDSA", test_payload())).has_value());
    ASSERT_TRUE(fetcher_.in_flight());
    verifier_.reset();
    EXPECT_EQ(fetcher_.cancels, 1);
    EXPECT_FALSE(fetcher_.in_flight());
}

TEST_F(JwksVerifierTest, DestructionCancelsTheArmedTimers) {
    CountingWaiter waiter;
    EXPECT_FALSE(verify(signed_token(ed_key(), "EdDSA", test_payload()), waiter).has_value());
    // Arms both the notification for the next iteration and the 15-minute refetch.
    fetcher_.respond(key_set({ed_key().jwk()}));
    verifier_.reset();
    // Either timer left armed would fire into freed memory here.
    pump();
    advance(minutes(20));
    EXPECT_EQ(waiter.calls, 0);
    EXPECT_EQ(fetcher_.requests, 1);
}

} // namespace
