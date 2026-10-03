#pragma once

#include "infra/curl/http.hpp"
#include "infra/curl/multi.hpp"

#include <array>
#include <cstddef>
#include <cstdint>
#include <curl/curl.h>
#include <expected>
#include <memory>

namespace infra::curl::detail {

// curl_global_init, once per process, before the first handle exists.
[[nodiscard]] bool global_init() noexcept;

// One request's easy handle plus everything libcurl points into while it runs: the header
// list, the error buffer, the response being received and the body source. It lives at a
// fixed address for that reason.
class Exchange {
    struct Token {
        explicit Token() = default;
    };

public:
    // On a reactor, a source that runs dry pauses the upload until resume_body(). In a
    // blocking curl_easy_perform nothing could resume it, so there it aborts the exchange.
    enum class Mode : std::uint8_t { Reactor, Blocking };

    // `source` supplies exactly `upload_length` bytes for PUT and POST; other methods ignore
    // both. With a `sink`, a 2xx body goes there instead of into the response.
    [[nodiscard]] static std::expected<std::unique_ptr<Exchange>, Failure>
    create(const Request& request, std::uint64_t upload_length, IBodySource* source,
           Mode mode = Mode::Reactor, IDownloadSink* sink = nullptr);

    Exchange(Token token, std::size_t max_body, std::uint64_t upload_length, IBodySource* source,
             Mode mode, IDownloadSink* sink) noexcept;
    Exchange(const Exchange&) = delete;
    Exchange& operator=(const Exchange&) = delete;
    ~Exchange() = default;

    [[nodiscard]] CURL* easy() const noexcept { return easy_.get(); }
    // Once libcurl reports the transfer done with `code`.
    [[nodiscard]] Result finish(CURLcode code);
    void resume_body() noexcept;

private:
    [[nodiscard]] std::expected<void, Failure> configure(const Request& request);

    static std::size_t on_header(char* data, std::size_t size, std::size_t count,
                                 void* self) noexcept;
    static std::size_t on_body(char* data, std::size_t size, std::size_t count,
                               void* self) noexcept;
    static std::size_t on_read(char* data, std::size_t size, std::size_t count,
                               void* self) noexcept;
    // Request::public_only: opens the socket for a global unicast address only.
    static curl_socket_t on_open_socket(void* self, curlsocktype purpose,
                                        curl_sockaddr* address) noexcept;

    struct SlistFree {
        void operator()(curl_slist* list) const noexcept { curl_slist_free_all(list); }
    };
    struct EasyCleanup {
        void operator()(CURL* easy) const noexcept { curl_easy_cleanup(easy); }
    };

    // Declared before the handle so it is freed after it: libcurl reads the list until
    // curl_easy_cleanup.
    std::unique_ptr<curl_slist, SlistFree> header_list_;
    std::unique_ptr<CURL, EasyCleanup> easy_;
    std::array<char, CURL_ERROR_SIZE> error_{};
    std::size_t max_body_;
    std::uint64_t upload_left_;
    IBodySource* source_;
    Mode mode_;
    IDownloadSink* sink_;
    bool paused_ = false;
    bool oversize_ = false;
    // An address was refused under Request::public_only.
    bool refused_ = false;
    Response response_;
};

} // namespace infra::curl::detail
