#include "fake_push_service.hpp"

#include "support/https_test_server.hpp"

#include <mutex>

namespace ulw::test {

struct FakePushService::State {
    struct Browser {
        infra::webpush::PrivateKey key;
        infra::webpush::AuthSecret auth;
    };

    Reply serve(const ServedRequest& r) {
        const std::scoped_lock lock(mutex);
        if (gone.contains(r.target)) {
            return Reply{.status = 410, .headers = {}, .body = {}};
        }
        const auto it = browsers.find(r.target);
        if (it == browsers.end()) {
            return Reply{.status = 404, .headers = {}, .body = {}};
        }
        const std::vector<std::uint8_t> body(r.body.begin(), r.body.end());
        const auto plain = infra::webpush::decrypt(it->second.key, it->second.auth, body);
        Delivered d{.path = r.target,
                    .headers = {},
                    .plaintext = plain ? std::string(plain->begin(), plain->end()) : "",
                    .decrypted = plain.has_value()};
        for (const auto& [name, value] : r.headers) {
            d.headers[name] = value;
        }
        delivered.push_back(std::move(d));
        return Reply{.status = 201, .headers = {{"Location", "/m/1"}}, .body = {}};
    }

    std::mutex mutex;
    std::map<std::string, Browser> browsers;
    std::map<std::string, bool> gone;
    std::vector<Delivered> delivered;
    std::unique_ptr<HttpsTestServer> server;
};

FakePushService::FakePushService() : state_(std::make_shared<State>()) {
    State* s = state_.get();
    state_->server =
        std::make_unique<HttpsTestServer>([s](const ServedRequest& r) { return s->serve(r); });
}

FakePushService::~FakePushService() {
    state_->server.reset();
}

void FakePushService::subscribe(const std::string& path, const infra::webpush::PrivateKey& key,
                                const infra::webpush::AuthSecret& auth) {
    const std::scoped_lock lock(state_->mutex);
    state_->browsers[path] = State::Browser{.key = key, .auth = auth};
}

void FakePushService::gone(const std::string& path) {
    const std::scoped_lock lock(state_->mutex);
    state_->gone[path] = true;
}

std::string FakePushService::base_url() const {
    return state_->server->base_url();
}

std::uint16_t FakePushService::port() const {
    return state_->server->port();
}

std::vector<FakePushService::Delivered> FakePushService::delivered() const {
    const std::scoped_lock lock(state_->mutex);
    return state_->delivered;
}

} // namespace ulw::test
