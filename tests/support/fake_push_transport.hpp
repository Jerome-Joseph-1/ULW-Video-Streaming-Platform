#pragma once

#include "infra/webpush/sender.hpp"

#include <cstddef>
#include <deque>
#include <optional>
#include <string>
#include <string_view>

namespace ulw::test {

// Records each POST and lets the test answer it when it likes.
class FakePushTransport final : public infra::webpush::IPushTransport {
public:
    struct Post {
        infra::webpush::PushRequest request;
        Done done;
    };

    [[nodiscard]] bool post(infra::webpush::PushRequest request, Done done) noexcept override {
        if (refuse_next) {
            refuse_next = false;
            return false;
        }
        posts.push_back(Post{.request = std::move(request), .done = std::move(done)});
        ++total;
        return true;
    }

    // Answers the oldest unanswered POST.
    void answer(infra::webpush::PushOutcome outcome) {
        Post p = std::move(posts.front());
        posts.pop_front();
        p.done(std::move(outcome));
    }
    void status(int code, std::optional<core::Seconds> retry_after = std::nullopt) {
        answer(infra::webpush::PushResponse{.status = code, .retry_after = retry_after});
    }

    std::deque<Post> posts;
    std::size_t total = 0;
    bool refuse_next = false;
};

// A request's header value by name, as the sender writes them ("Name: value").
inline std::optional<std::string_view> push_header(const infra::webpush::PushRequest& r,
                                                   std::string_view name) {
    for (const std::string& line : r.headers) {
        if (line.starts_with(name) && line.size() > name.size() + 1 && line[name.size()] == ':') {
            return std::string_view(line).substr(name.size() + 2);
        }
    }
    return std::nullopt;
}

} // namespace ulw::test
