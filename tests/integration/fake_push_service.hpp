#pragma once

#include "infra/webpush/ece.hpp"

#include <cstdint>
#include <map>
#include <memory>
#include <string>
#include <vector>

namespace ulw::test {

// A push service on loopback, over TLS with the test CA: it decrypts each message with the
// subscription's private key, as the browser would, and answers 201; a path it was told is gone
// is answered 410, and any other 404.
class FakePushService {
public:
    struct Delivered {
        std::string path;
        // Names in lower case.
        std::map<std::string, std::string> headers;
        // Empty when the message did not decrypt.
        std::string plaintext;
        bool decrypted = false;
    };

    FakePushService();
    ~FakePushService();
    FakePushService(const FakePushService&) = delete;
    FakePushService& operator=(const FakePushService&) = delete;

    // A browser's subscription at `path`, which only it can read.
    void subscribe(const std::string& path, const infra::webpush::PrivateKey& key,
                   const infra::webpush::AuthSecret& auth);
    void gone(const std::string& path);

    [[nodiscard]] std::string base_url() const;
    [[nodiscard]] std::uint16_t port() const;
    [[nodiscard]] std::vector<Delivered> delivered() const;

private:
    struct State;
    std::shared_ptr<State> state_;
};

} // namespace ulw::test
