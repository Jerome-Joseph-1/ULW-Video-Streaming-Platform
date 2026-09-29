#pragma once

#include "infra/curl/http.hpp"
#include "net/reactor.hpp"

#include <cstddef>
#include <cstdint>
#include <expected>
#include <memory>
#include <span>

namespace infra::curl {

namespace detail {
class Exchange;
} // namespace detail

enum class MultiError : std::uint8_t {
    InitFailed,
    // This libcurl resolves names on the calling thread, which would block the reactor.
    SynchronousResolver,
};

class Transfer;

// Runs libcurl transfers on a reactor: every socket libcurl opens is watched through the
// reactor, and libcurl's one timeout is one reactor timer. Everything here, handlers included,
// belongs to the reactor thread.
class Multi {
    struct Token {
        explicit Token() = default;
    };

public:
    class Impl;

    // A ceiling on descriptors and on the sockets the store holds open for us. Transfers beyond
    // it wait in libcurl's queue for a connection to come free rather than failing.
    static constexpr std::size_t kDefaultMaxConnections = 64;

    [[nodiscard]] static std::expected<std::unique_ptr<Multi>, MultiError>
    create(net::IReactor& reactor, std::size_t max_connections = kDefaultMaxConnections);

    Multi(Token token, std::unique_ptr<Impl> impl) noexcept;
    // Every transfer must be destroyed first.
    ~Multi();
    Multi(const Multi&) = delete;
    Multi& operator=(const Multi&) = delete;

private:
    friend class Transfer;

    std::unique_ptr<Impl> impl_;
};

class ITransferHandler {
public:
    virtual ~ITransferHandler() = default;
    // Once per transfer, on the reactor thread, outside every libcurl callback and never from
    // inside a Transfer call. The transfer is already detached, so the handler may destroy it.
    virtual void on_transfer_done(Result result) noexcept = 0;
};

// A streamed request body. Called from inside libcurl, on the reactor thread.
class IBodySource {
public:
    virtual ~IBodySource() = default;
    // Copies at most out.size() bytes and returns how many. 0 means "nothing yet" and pauses
    // the upload until Transfer::resume_body(); it cannot end the body, whose length is fixed
    // when the transfer starts.
    virtual std::size_t read_body(std::span<std::byte> out) noexcept = 0;
};

class Transfer {
    struct Token {
        explicit Token() = default;
    };

public:
    [[nodiscard]] static std::expected<std::unique_ptr<Transfer>, Failure>
    start(Multi& multi, const Request& request, ITransferHandler& handler);
    // PUT or POST of exactly `length` bytes pulled from `body` as libcurl can send them.
    [[nodiscard]] static std::expected<std::unique_ptr<Transfer>, Failure>
    start_upload(Multi& multi, const Request& request, std::uint64_t length, IBodySource& body,
                 ITransferHandler& handler);

    Transfer(Token token, Multi::Impl& multi, std::unique_ptr<detail::Exchange> exchange,
             ITransferHandler& handler) noexcept;
    // Cancels a transfer that is still running; its handler is not called.
    ~Transfer();
    Transfer(const Transfer&) = delete;
    Transfer& operator=(const Transfer&) = delete;

    // Tells a paused upload that read_body() has bytes again. read_body() may run before this
    // returns.
    void resume_body() noexcept;

private:
    friend class Multi::Impl;

    [[nodiscard]] static std::expected<std::unique_ptr<Transfer>, Failure>
    launch(Multi& multi, std::unique_ptr<detail::Exchange> exchange, ITransferHandler& handler);

    Multi::Impl& multi_;
    std::unique_ptr<detail::Exchange> exchange_;
    ITransferHandler& handler_;
};

} // namespace infra::curl
