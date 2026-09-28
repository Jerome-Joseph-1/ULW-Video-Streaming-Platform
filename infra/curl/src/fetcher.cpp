#include "infra/curl/fetcher.hpp"

#include <algorithm>
#include <expected>
#include <memory>
#include <string>
#include <utility>

namespace infra::curl {

class HttpFetcher::Fetch final : public ITransferHandler {
public:
    Fetch(HttpFetcher& owner, Callback done) noexcept : owner_(owner), done_(std::move(done)) {}

    void on_transfer_done(Result result) noexcept override {
        owner_.finished(*this, std::move(result));
    }

    void hold(std::unique_ptr<Transfer> transfer) noexcept { transfer_ = std::move(transfer); }
    [[nodiscard]] Callback take_callback() noexcept { return std::move(done_); }

private:
    HttpFetcher& owner_;
    Callback done_;
    std::unique_ptr<Transfer> transfer_;
};

HttpFetcher::HttpFetcher(Multi& multi) noexcept : multi_(multi) {}

HttpFetcher::~HttpFetcher() = default;

std::expected<void, Failure> HttpFetcher::get(std::string url, std::size_t max_body,
                                              Callback done) {
    auto fetch = std::make_unique<Fetch>(*this, std::move(done));
    auto transfer = Transfer::start(
        multi_,
        Request{.method = Method::Get, .url = std::move(url), .headers = {}, .max_body = max_body},
        *fetch);
    if (!transfer) {
        return std::unexpected(std::move(transfer.error()));
    }
    fetch->hold(std::move(*transfer));
    fetches_.push_back(std::move(fetch));
    return {};
}

void HttpFetcher::finished(Fetch& fetch, Result result) noexcept {
    Callback done = fetch.take_callback();
    // Destroys the fetch and its transfer, which libcurl has already let go of.
    std::erase_if(fetches_, [&](const auto& f) { return f.get() == &fetch; });
    // Last: the callback may destroy this fetcher.
    done(std::move(result));
}

} // namespace infra::curl
