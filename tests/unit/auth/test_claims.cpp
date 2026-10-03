#include "test_claims.hpp"

#include "core/util/time.hpp"

#include "support/fake_clock.hpp"

#include <chrono>
#include <cstdint>
#include <initializer_list>
#include <string>
#include <utility>
#include <vector>

namespace ulw::test {

std::string numeric_date(std::int64_t offset_seconds) {
    const FakeClock clock;
    const std::int64_t now =
        std::chrono::duration_cast<core::Seconds>(clock.wall_now().time_since_epoch()).count();
    return std::to_string(now + offset_seconds);
}

std::string test_payload(std::initializer_list<ClaimChange> changes) {
    std::vector<std::pair<std::string, std::string>> members{
        {"iss", R"("https://id.example.com")"},
        {"aud", R"("ulw-test-audience")"},
        {"sub", R"("alice")"},
        {"email", R"("alice@example.com")"},
        {"exp", numeric_date(3600)},
    };
    for (const auto& [name, value] : changes) {
        std::erase_if(members, [&](const auto& m) { return m.first == name; });
        if (value) {
            members.emplace_back(name, *value);
        }
    }
    std::string out = "{";
    for (const auto& [name, value] : members) {
        out += out.size() > 1 ? ",\"" : "\"";
        out += name;
        out += "\":";
        out += value;
    }
    return out + '}';
}

} // namespace ulw::test
