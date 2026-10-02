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

const TestKey& next_rsa_key() {
    static const TestKey key = TestKey::rsa("rsa-2");
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

// What Askedin's auth-service issues (docs/integration/auth.md, Askedin): RS256 under a header
// of alg, kid and typ only; aud an array of one; sub a lowercase UUID, repeated as uid; jti equal
// to the kid; tid, perms and sid besides; no nbf; exp an hour after iat. Its 2FA challenge and
// PAT swap tokens are the same but for `aud` and a `typ` claim.
constexpr std::string_view kAskedinUser = "0b5e4b1e-7c1a-4d8e-9f3a-2c6d8e1f4a7b";

std::string askedin_token(const TestKey& key, const std::string& aud = R"(["askedin-platform"])",
                          std::optional<std::string> typ = std::nullopt,
                          std::string_view sub = kAskedinUser) {
    const std::string quoted_sub = '"' + std::string(sub) + '"';
    const std::string header = R"({"alg":"RS256","kid":")" + key.kid() + R"(","typ":"JWT"})";
    const std::string payload = test_payload({{"aud", aud},
                                              {"sub", quoted_sub},
                                              {"uid", quoted_sub},
                                              {"iat", ulw::test::numeric_date(0)},
                                              {"jti", '"' + key.kid() + '"'},
                                              {"tid", R"("t-1")"},
                                              {"perms", R"(["video:upload"])"},
                                              {"sid", R"("s-1")"},
                                              {"typ", std::move(typ)}});
    const std::string input = encode_base64url(header) + '.' + encode_base64url(payload);
    return input + '.' + encode_base64url(key.sign("RS256", input));
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

// A kid republished with other key material is a withdrawal of the old key, and the verdicts it
// produced go with it.
TEST_F(JwksVerifierTest, AKeyReplacedUnderItsKidTakesItsVerdictsWithIt) {
    const std::string old_token = signed_token(ed_key(), "EdDSA", test_payload());
    EXPECT_TRUE(verify_through_fetch(old_token, key_set({ed_key().jwk()})));
    advance(seconds(11));

    static const TestKey impostor = TestKey::ed25519(ed_key().kid());
    const std::string new_token = signed_token(next_ed_key(), "EdDSA", test_payload());
    EXPECT_TRUE(verify_through_fetch(new_token, key_set({impostor.jwk(), next_ed_key().jwk()})));
    EXPECT_EQ(verify(old_token), refused(AuthError::BadSignature));
}

// A remembered unknown kid refuses that kid only; another unseen kid still gets its fetch.
TEST_F(JwksVerifierTest, ARememberedUnknownKidRefusesOnlyItself) {
    const std::string jwks = key_set({ed_key().jwk()});
    EXPECT_TRUE(verify_through_fetch(signed_token(ed_key(), "EdDSA", test_payload()), jwks));
    advance(seconds(11));
    // Sorts after every kid below, so no ordering of kids can stand in for equality.
    EXPECT_EQ(verify_through_fetch(token_with_kid("zz-stranger"), jwks),
              refused(AuthError::UnknownKey));
    advance(seconds(11));
    EXPECT_TRUE(verify_through_fetch(signed_token(next_ed_key(), "EdDSA", test_payload()),
                                     key_set({ed_key().jwk(), next_ed_key().jwk()})));
    EXPECT_EQ(fetcher_.requests, 3);
}

// Unknown kids are remembered 64 at a time: the 65th pushes out the oldest, which may then be
// looked up again.
TEST_F(JwksVerifierTest, TheMemoryOfUnknownKidsIsBounded) {
    const std::string jwks = key_set({ed_key().jwk()});
    CountingWaiter waiter;
    for (int i = 0; i < 64; ++i) {
        EXPECT_FALSE(verify(token_with_kid("junk-" + std::to_string(i)), waiter).has_value()) << i;
    }
    EXPECT_EQ(fetcher_.requests, 1);
    fetcher_.respond(jwks);
    pump();
    EXPECT_EQ(verify(token_with_kid("junk-0")), refused(AuthError::UnknownKey));
    advance(seconds(11));
    EXPECT_EQ(verify_through_fetch(token_with_kid("junk-64"), jwks),
              refused(AuthError::UnknownKey));
    EXPECT_EQ(verify(token_with_kid("junk-1")), refused(AuthError::UnknownKey));
    advance(seconds(11));
    // Still inside the minute it would have been remembered for, but forgotten to make room.
    EXPECT_FALSE(verify(token_with_kid("junk-0")).has_value());
    EXPECT_EQ(fetcher_.requests, 3);
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

// A key Askedin withdraws while its JWKS cannot be reached must not verify for ever: after a
// day without a successful fetch the keys, and every verdict they gave, are dropped.
TEST_F(JwksVerifierTest, KeysUnrefreshedForADayAreDroppedAndEveryTokenRefusedUntilAFetchLands) {
    std::vector<core::Millis> expiries;
    verifier_ = std::make_unique<JwksVerifier>(
        *reactor_, fetcher_,
        JwksConfig{.url = std::string(kUrl),
                   .claims = ulw::test::kTestRules,
                   .on_keys_expired = [&expiries](core::Millis age) noexcept {
                       expiries.push_back(age);
                   }});
    const std::string token =
        signed_token(ed_key(), "EdDSA",
                     test_payload({{"exp", ulw::test::numeric_date(std::int64_t{3} * 24 * 3600)}}));
    EXPECT_TRUE(verify_through_fetch(token, key_set({ed_key().jwk()})));
    EXPECT_FALSE(verifier_->keys_expired());

    // Every refetch fails from here: the endpoint is down, or blocked.
    const auto fail_fetches_for = [&](core::Millis span) {
        const core::MonoTime until = clock_.now() + span;
        while (clock_.now() < until) {
            if (fetcher_.in_flight()) {
                fetcher_.respond(std::nullopt);
            }
            advance(std::min<core::Millis>(
                seconds(30), std::chrono::duration_cast<core::Millis>(until - clock_.now())));
        }
    };
    fail_fetches_for(std::chrono::hours(24) - seconds(1));
    EXPECT_TRUE(verify(token).value_or(refused(AuthError::KeysUnavailable)).has_value());
    EXPECT_FALSE(verifier_->keys_expired());
    EXPECT_TRUE(expiries.empty());

    fail_fetches_for(seconds(1));
    EXPECT_EQ(verify(token), refused(AuthError::KeysUnavailable));
    EXPECT_TRUE(verifier_->keys_expired());
    ASSERT_EQ(expiries.size(), 1U);
    EXPECT_GE(expiries.front(), std::chrono::hours(24));
    // A token never seen before fares no better.
    EXPECT_EQ(verify(signed_token(ed_key(), "EdDSA", test_payload({{"sub", "\"bob\""}}))),
              refused(AuthError::KeysUnavailable));

    // The next fetch that lands puts the keys back.
    while (!fetcher_.in_flight()) {
        advance(seconds(30));
    }
    fetcher_.respond(key_set({ed_key().jwk()}));
    EXPECT_FALSE(verifier_->keys_expired());
    EXPECT_TRUE(verify(token).value_or(refused(AuthError::KeysUnavailable)).has_value());
    EXPECT_EQ(expiries.size(), 1U);
}

TEST_F(JwksVerifierTest, HowLongKeysStayTrustedIsConfigurable) {
    verifier_ = std::make_unique<JwksVerifier>(*reactor_, fetcher_,
                                               JwksConfig{.url = std::string(kUrl),
                                                          .claims = ulw::test::kTestRules,
                                                          .max_key_age = std::chrono::hours(1)});
    const std::string token = signed_token(ed_key(), "EdDSA", test_payload());
    const core::MonoTime start = clock_.now();
    EXPECT_TRUE(verify_through_fetch(token, key_set({ed_key().jwk()})));
    for (int i = 0; i < 240 && !verifier_->keys_expired(); ++i) {
        if (fetcher_.in_flight()) {
            fetcher_.respond(std::nullopt);
        }
        advance(seconds(30));
        static_cast<void>(verify(token));
    }
    EXPECT_TRUE(verifier_->keys_expired());
    EXPECT_GE(clock_.now() - start, std::chrono::hours(1));
    EXPECT_LE(clock_.now() - start, std::chrono::hours(1) + seconds(30));
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

// Askedin's access tokens as its auth-service issues them verify, and the user is their sub.
TEST_F(JwksVerifierTest, AnAskedinAccessTokenVerifies) {
    const VerifyResult verified =
        verify_through_fetch(askedin_token(rsa_key()), key_set({rsa_key().jwk()}));
    ASSERT_TRUE(verified.has_value());
    EXPECT_EQ(verified->subject.view(), "0b5e4b1e-7c1a-4d8e-9f3a-2c6d8e1f4a7b");
}

// Askedin signs its 2FA challenges and PAT swaps with the same key as its access tokens; only the
// audience tells them apart, and neither may authenticate here under the default audience.
TEST_F(JwksVerifierTest, AskedinsTwoFactorAndPatTokensAreRefusedForTheirAudience) {
    ASSERT_EQ(ulw::test::kTestRules.audience, "askedin-platform") << "not the default audience";
    const std::string jwks = key_set({rsa_key().jwk()});
    EXPECT_EQ(verify_through_fetch(
                  askedin_token(rsa_key(), R"(["askedin-2fa"])", R"("2fa_challenge")"), jwks),
              refused(AuthError::WrongAudience));
    EXPECT_EQ(verify(askedin_token(rsa_key(), R"(["askedin-pat"])", R"("pat")")),
              refused(AuthError::WrongAudience));
    EXPECT_EQ(verify(askedin_token(rsa_key(), R"("askedin-2fa")", R"("2fa_challenge")")),
              refused(AuthError::WrongAudience));
    EXPECT_EQ(verify(askedin_token(rsa_key(), R"("askedin-pat")", R"("pat")")),
              refused(AuthError::WrongAudience));
    // The same token under the platform's audience verifies: the audience alone refused them.
    EXPECT_TRUE(verify(askedin_token(rsa_key(), R"(["askedin-platform"])", R"("pat")"))
                    .value_or(refused(AuthError::KeysUnavailable)));
    EXPECT_EQ(fetcher_.requests, 1);
}

// Askedin rotates without overlap: the old kid leaves the set the moment the new key is made. A
// token verified before goes on verifying from the remembered verdict, and its key from the
// cached set, until the next refetch.
TEST_F(JwksVerifierTest, AWithdrawnKeyGoesOnVerifyingUntilTheNextRefetchWithoutADrop) {
    const std::string old_token = askedin_token(rsa_key());
    EXPECT_TRUE(verify_through_fetch(old_token, key_set({rsa_key().jwk()})));
    // Rotated now: the published set holds next_rsa_key() alone, and nobody has asked for it.
    advance(minutes(15) - seconds(1));
    EXPECT_TRUE(verify(old_token).value_or(refused(AuthError::KeysUnavailable)));
    EXPECT_EQ(fetcher_.requests, 1);
    advance(milliseconds(1200));
    ASSERT_EQ(fetcher_.requests, 2);
    fetcher_.respond(key_set({next_rsa_key().jwk()}));
    EXPECT_EQ(verify(old_token), refused(AuthError::UnknownKey));
}

// Askedin's jti is its kid, the same for every token under a key: two users' tokens with one jti
// are answered each as itself, from the signature and from the cache alike.
TEST_F(JwksVerifierTest, TokensSharingAJtiAreAnsweredEachAsItsOwnUser) {
    constexpr std::string_view kOther = "7d0c6a52-3b9e-4f1d-8a2c-5e6f7a8b9c0d";
    const std::string first = askedin_token(rsa_key());
    const std::string second =
        askedin_token(rsa_key(), R"(["askedin-platform"])", std::nullopt, kOther);
    const auto subject = [&](const std::string& token) {
        const VerifyResult r = verify(token).value_or(refused(AuthError::KeysUnavailable));
        return r ? std::string(r->subject.view()) : std::string("refused");
    };
    const VerifyResult verified = verify_through_fetch(first, key_set({rsa_key().jwk()}));
    ASSERT_TRUE(verified.has_value());
    EXPECT_EQ(verified->subject.view(), kAskedinUser);
    EXPECT_EQ(subject(second), kOther);
    // Both answered from the cache now.
    EXPECT_EQ(subject(first), kAskedinUser);
    EXPECT_EQ(subject(second), kOther);
    EXPECT_EQ(fetcher_.requests, 1);
}

// What SIGHUP does: a fetch starts at once, the cached keys and verdicts keep answering while it
// runs, and when it lands a token whose key left the set is refused.
TEST_F(JwksVerifierTest, DroppingTheCachesRefusesATokenWhoseKeyLeftTheSet) {
    const std::string old_token = askedin_token(rsa_key());
    EXPECT_TRUE(verify_through_fetch(old_token, key_set({rsa_key().jwk()})));
    ASSERT_EQ(fetcher_.requests, 1);
    EXPECT_FALSE(verifier_->drop_pending());

    verifier_->drop_caches();
    EXPECT_EQ(fetcher_.requests, 2) << "no fetch started by the drop";
    ASSERT_TRUE(fetcher_.in_flight());
    EXPECT_TRUE(verifier_->drop_pending());
    const std::optional<VerifyResult> during = verify(old_token);
    ASSERT_TRUE(during.has_value()) << "waited on the drop's fetch";
    EXPECT_TRUE(during->has_value());

    fetcher_.respond(key_set({next_rsa_key().jwk()}));
    EXPECT_FALSE(verifier_->drop_pending());
    EXPECT_EQ(verify(old_token), refused(AuthError::UnknownKey));
    // The new key came with that fetch.
    EXPECT_TRUE(
        verify(askedin_token(next_rsa_key())).value_or(refused(AuthError::KeysUnavailable)));
    EXPECT_EQ(fetcher_.requests, 2);
}

// The drop forgets every verdict even when the key stayed, so the next answer is a signature
// check again, and still a pass.
TEST_F(JwksVerifierTest, AfterADropATokenStillPublishedVerifiesAgain) {
    const std::string token = askedin_token(rsa_key());
    EXPECT_TRUE(verify_through_fetch(token, key_set({rsa_key().jwk()})));
    verifier_->drop_caches();
    EXPECT_TRUE(verify(token).value_or(refused(AuthError::KeysUnavailable)));
    fetcher_.respond(key_set({rsa_key().jwk()}));
    EXPECT_FALSE(verifier_->drop_pending());
    EXPECT_TRUE(verify(token).value_or(refused(AuthError::KeysUnavailable)));
    EXPECT_EQ(fetcher_.requests, 2);
}

// A drop during a fetch replaces it: what that fetch would bring may predate the rotation.
TEST_F(JwksVerifierTest, ADropDuringAFetchCancelsItAndStartsAnother) {
    CountingWaiter waiter;
    EXPECT_FALSE(verify(askedin_token(rsa_key()), waiter).has_value());
    ASSERT_EQ(fetcher_.requests, 1);
    verifier_->drop_caches();
    EXPECT_EQ(fetcher_.cancels, 1);
    EXPECT_EQ(fetcher_.requests, 2);
    fetcher_.respond(key_set({rsa_key().jwk()}));
    EXPECT_FALSE(verifier_->drop_pending());
    pump();
    EXPECT_EQ(waiter.calls, 1);
    EXPECT_TRUE(
        verify(askedin_token(rsa_key()), waiter).value_or(refused(AuthError::KeysUnavailable)));
}

// The drop's fetch forgets the kids remembered as unknown before it, so one may be looked up
// again; a kid sought during that fetch and missing from it is still remembered.
TEST_F(JwksVerifierTest, ADropForgetsUnknownKidsButRemembersTheOnesItsFetchMissed) {
    const std::string remembered = askedin_token(next_rsa_key());
    EXPECT_EQ(verify_through_fetch(remembered, key_set({rsa_key().jwk()})),
              refused(AuthError::UnknownKey));
    verifier_->drop_caches();
    // Still remembered while the drop's fetch runs: answered at once.
    EXPECT_EQ(verify(remembered), refused(AuthError::UnknownKey));
    CountingWaiter waiter;
    EXPECT_FALSE(verify(token_with_kid("sought"), waiter).has_value());
    fetcher_.respond(key_set({rsa_key().jwk()}));
    pump();
    EXPECT_EQ(waiter.calls, 1);
    advance(seconds(11));
    EXPECT_EQ(verify(token_with_kid("sought")), refused(AuthError::UnknownKey));
    EXPECT_EQ(fetcher_.requests, 2);
    // Inside the minute it was remembered for, but forgotten with the drop: it gets a fetch.
    EXPECT_FALSE(verify(remembered).has_value());
    EXPECT_EQ(fetcher_.requests, 3);
}

// With the endpoint down the drop waits: the keys in hand go on answering, the old key's tokens
// included, and the drop completes with the first retry that succeeds. The retries back off from
// 1 s again, however many failures came before the drop.
TEST_F(JwksVerifierTest, ADropWhoseFetchFailsKeepsTheKeysUntilARetryCompletesIt) {
    const std::string old_token = askedin_token(rsa_key());
    EXPECT_TRUE(verify_through_fetch(old_token, key_set({rsa_key().jwk()})));
    // Three failed refetches: the next retry would be 4 s away.
    advance(minutes(15) + milliseconds(200));
    ASSERT_EQ(fetcher_.requests, 2);
    fetcher_.respond(std::nullopt);
    advance(seconds(1) + milliseconds(200));
    fetcher_.respond(std::nullopt);
    advance(seconds(2) + milliseconds(200));
    fetcher_.respond(std::nullopt);
    ASSERT_EQ(fetcher_.requests, 4);

    verifier_->drop_caches();
    ASSERT_EQ(fetcher_.requests, 5);
    fetcher_.respond("<html>not a key set</html>");
    EXPECT_TRUE(verifier_->drop_pending());
    EXPECT_TRUE(verify(old_token).value_or(refused(AuthError::KeysUnavailable)))
        << "the keys in hand went with a failed fetch";
    EXPECT_FALSE(verifier_->keys_expired());

    advance(seconds(1) + milliseconds(200));
    ASSERT_EQ(fetcher_.requests, 6) << "the retry did not back off from 1 s";
    fetcher_.respond(key_set({next_rsa_key().jwk()}));
    EXPECT_FALSE(verifier_->drop_pending());
    EXPECT_EQ(verify(old_token), refused(AuthError::UnknownKey));
}

} // namespace
