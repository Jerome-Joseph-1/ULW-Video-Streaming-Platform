#include "net/offload_pool.hpp"
#include "net/transport.hpp"

#include <algorithm>
#include <array>
#include <cerrno>
#include <charconv>
#include <cstdio>
#include <openssl/bio.h>
#include <openssl/buffer.h>
#include <openssl/err.h>
#include <openssl/ssl.h>
#include <string_view>
#include <utility>

namespace net {

namespace {

constexpr core::Millis kHandshakeTimeout{5'000};
// The largest plaintext one TLS record carries (RFC 8446 section 5.1), so one SSL_read_ex never
// has more to return.
constexpr std::size_t kRecordPlaintext = std::size_t{16} * 1024;
// Ciphertext goes into the read BIO a piece at a time and is read out before the next piece. A
// memory BIO keeps the largest size it ever held, so feeding a whole 64 KiB receive at once
// would leave every connection holding 64 KiB of BIO for good.
constexpr std::size_t kCipherPiece = kRecordPlaintext;
// A piece plus the partial record left from the one before: 16 KiB + at most 16 KiB of
// plaintext, 2 KiB of TLS 1.2 expansion and a 5-byte header, under 48 KiB. Only a paused
// protocol, which parks the rest of a receive in the BIO, grows it past that, and the BIO is
// replaced once that backlog has been read.
constexpr std::size_t kReadBioKeep = std::size_t{48} * 1024;
// A session id context is required for resumption to be offered at all; the value only has to
// be the same for every connection this server resumes.
constexpr std::string_view kSessionContext = "ulw-gateway";
// Forward secrecy and AEAD only, for clients that stop at TLS 1.2; TLS 1.3 suites are all
// both already.
constexpr const char* kTls12Ciphers = "ECDHE+AESGCM:ECDHE+CHACHA20";

struct SslCtxFree {
    void operator()(SSL_CTX* ctx) const noexcept { SSL_CTX_free(ctx); }
};
struct SslFree {
    void operator()(SSL* ssl) const noexcept { SSL_free(ssl); }
};
using SslCtxPtr = std::unique_ptr<SSL_CTX, SslCtxFree>;
using SslPtr = std::unique_ptr<SSL, SslFree>;

// Appends to a fixed line, dropping what does not fit: logging must not allocate or throw on
// the reactor thread.
class LogLine {
public:
    void append(std::string_view text) noexcept {
        const std::size_t n = std::min(text.size(), line_.size() - 1 - used_);
        std::copy_n(text.begin(), n, line_.begin() + static_cast<std::ptrdiff_t>(used_));
        used_ += n;
    }
    void write() noexcept {
        line_.at(used_) = '\n';
        static_cast<void>(std::fwrite(line_.data(), 1, used_ + 1, stderr));
    }

private:
    std::array<char, 1024> line_{};
    std::size_t used_ = 0;
};

// What every connection of one factory shares.
struct Shared {
    std::size_t handshakes_in_flight = 0;
    std::uint64_t handshake_failures = 0;
    // Failures are logged at most once a second. Anyone can open connections and fail their
    // handshakes, and a line per failure would let a scanner fill the log and spend the loop
    // on write(2); the counter above still sees every one.
    core::MonoTime next_log;
    std::uint64_t unlogged = 0;
};

constexpr core::Millis kLogInterval{1'000};

// Everything OpenSSL queued, on one line, so one failure reads as one event.
void log_ssl_errors(Shared& shared, core::MonoTime now, std::string_view what) noexcept {
    if (now < shared.next_log) {
        ++shared.unlogged;
        ERR_clear_error();
        return;
    }
    shared.next_log = now + kLogInterval;
    LogLine line;
    line.append("tls: ");
    line.append(what);
    std::array<char, 256> reason{};
    while (const unsigned long e = ERR_get_error()) {
        ERR_error_string_n(e, reason.data(), reason.size());
        line.append(": ");
        line.append(reason.data());
    }
    if (const std::uint64_t n = std::exchange(shared.unlogged, 0); n != 0) {
        std::array<char, 24> count{};
        auto* const end = std::to_chars(count.begin(), count.end(), n).ptr;
        line.append(" (");
        line.append({count.begin(), end});
        line.append(" more not logged)");
    }
    line.write();
}

std::string queued_errors() {
    std::string out;
    std::array<char, 256> reason{};
    while (const unsigned long e = ERR_get_error()) {
        ERR_error_string_n(e, reason.data(), reason.size());
        out.append(out.empty() ? "" : "; ").append(reason.data());
    }
    return out.empty() ? "unknown error" : out;
}

std::expected<SslCtxPtr, std::string> make_context(const TlsFiles& files) {
    ERR_clear_error();
    SslCtxPtr ctx{SSL_CTX_new(TLS_server_method())};
    if (!ctx) {
        return std::unexpected("tls context: " + queued_errors());
    }
    SSL_CTX* c = ctx.get();
    ERR_clear_error();
    if (SSL_CTX_set_min_proto_version(c, TLS1_2_VERSION) != 1 ||
        SSL_CTX_set_cipher_list(c, kTls12Ciphers) != 1) {
        return std::unexpected("tls context: " + queued_errors());
    }
    // Partial writes let one SSL_write_ex return after a record; the moving buffer lets a retry
    // pass a different pointer; released buffers give back the 16 KiB read and write buffers
    // of every connection that is idle between records.
    SSL_CTX_set_mode(c, SSL_MODE_ENABLE_PARTIAL_WRITE | SSL_MODE_ACCEPT_MOVING_WRITE_BUFFER |
                            SSL_MODE_RELEASE_BUFFERS);
    SSL_CTX_set_options(c, SSL_OP_NO_RENEGOTIATION | SSL_OP_CIPHER_SERVER_PREFERENCE |
                               SSL_OP_IGNORE_UNEXPECTED_EOF);
    SSL_CTX_clear_options(c, SSL_OP_NO_TICKET);
    // 0-RTT data can be replayed, and an upload chunk replayed is an upload chunk applied
    // twice.
    ERR_clear_error();
    if (SSL_CTX_set_max_early_data(c, 0) != 1 || SSL_CTX_set_recv_max_early_data(c, 0) != 1) {
        return std::unexpected("tls context: " + queued_errors());
    }
    ERR_clear_error();
    // OpenSSL takes the context as unsigned bytes; the text is ASCII.
    const auto* context = reinterpret_cast<const unsigned char*>(kSessionContext.data());
    if (SSL_CTX_set_session_id_context(c, context,
                                       static_cast<unsigned int>(kSessionContext.size())) != 1) {
        return std::unexpected("tls context: " + queued_errors());
    }
    // Tickets carry the session to the client, so resumption needs nothing kept here. A
    // server-side cache would hold up to 20,480 sessions of heap on a box whose memory is
    // budgeted per connection.
    SSL_CTX_set_session_cache_mode(c, SSL_SESS_CACHE_OFF);

    ERR_clear_error();
    if (SSL_CTX_use_certificate_chain_file(c, files.certificate_chain.c_str()) != 1) {
        return std::unexpected("certificate chain " + files.certificate_chain + ": " +
                               queued_errors());
    }
    ERR_clear_error();
    // Also checks the key against the certificate loaded above.
    if (SSL_CTX_use_PrivateKey_file(c, files.private_key.c_str(), SSL_FILETYPE_PEM) != 1) {
        return std::unexpected("private key " + files.private_key + ": " + queued_errors());
    }
    return ctx;
}

// Tickets issued before a reload keep resuming after it: a new context draws new ticket keys,
// and every returning client would pay a full handshake at once.
void carry_ticket_keys(SSL_CTX* from, SSL_CTX* to) noexcept {
    // OpenSSL 3 ticket keys: a 16-byte name, a 32-byte HMAC key and a 32-byte AES key.
    std::array<unsigned char, 80> keys{};
    if (SSL_CTX_get_tlsext_ticket_keys(from, keys.data(), keys.size()) == 1) {
        static_cast<void>(SSL_CTX_set_tlsext_ticket_keys(to, keys.data(), keys.size()));
    }
    OPENSSL_cleanse(keys.data(), keys.size());
}

// The memory-BIO model (ADR-0001): the reactor owns the socket, ciphertext it read goes into the
// read BIO, and whatever OpenSSL writes into the write BIO goes to the reactor's send queue.
// OpenSSL never touches the descriptor.
class TlsTransport final : public ITransport, public IStreamHandler, public ITimerHandler {
public:
    TlsTransport(Shared& shared, IReactor& reactor, IStreamHandler& upper, SslPtr ssl) noexcept
        : shared_(shared), reactor_(reactor), upper_(upper), ssl_(std::move(ssl)) {}

    ~TlsTransport() override {
        cancel_timer();
        end_handshake();
    }
    TlsTransport(const TlsTransport&) = delete;
    TlsTransport& operator=(const TlsTransport&) = delete;
    TlsTransport(TlsTransport&&) = delete;
    TlsTransport& operator=(TlsTransport&&) = delete;

    void start(ConnId conn) {
        conn_ = conn;
        counted_ = true;
        ++shared_.handshakes_in_flight;
        arm_timer(kHandshakeTimeout);
        SSL_set_accept_state(ssl_.get());
        sync_receiving();
    }

    void start_receiving() noexcept override {
        upper_receiving_ = true;
        // Records parked while the protocol was paused are handed over from the loop, not from
        // inside this call, which the protocol makes from its own callbacks. The socket stays
        // unread until they are gone: the reactor dispatches I/O before timers, so a receive
        // would otherwise land on top of the backlog each time the protocol pauses and resumes,
        // and the read BIO would grow by most of a receive per cycle.
        draining_ = state_ == State::Open && buffered();
        if (draining_ && !timer_armed_) {
            arm_timer(core::Millis{0});
        }
        sync_receiving();
    }

    void stop_receiving() noexcept override {
        upper_receiving_ = false;
        sync_receiving();
    }

    void send(std::span<const std::byte> bytes) noexcept override {
        if (state_ != State::Open || write_shut_) {
            return;
        }
        while (!bytes.empty()) {
            std::size_t n = 0;
            ERR_clear_error();
            if (SSL_write_ex(ssl_.get(), bytes.data(), bytes.size(), &n) != 1) {
                // A memory BIO never blocks, so a failed write is a broken session. Reported
                // from the loop: the caller is in the middle of its own work.
                log_ssl_errors(shared_, reactor_.now(), "write failed");
                defer_error(EPROTO);
                return;
            }
            bytes = bytes.subspan(n);
            flush();
        }
    }

    [[nodiscard]] std::size_t pending_send_bytes() const noexcept override {
        // The write BIO is flushed after every call that can fill it.
        return reactor_.pending_send_bytes(conn_);
    }

    void shutdown_write() noexcept override {
        if (write_shut_) {
            return;
        }
        write_shut_ = true;
        if (state_ == State::Open) {
            ERR_clear_error();
            // 0 means close_notify is written and the peer's has not arrived, which is all
            // that is asked for; the read side stays open.
            if (SSL_shutdown(ssl_.get()) < 0) {
                ERR_clear_error();
            }
            flush();
        }
        reactor_.shutdown_write(conn_);
    }

    void begin_close() noexcept override {
        state_ = State::Closed;
        cancel_timer();
        end_handshake();
        reactor_.begin_close(conn_);
    }

    [[nodiscard]] bool is_quiescent() const noexcept override {
        return reactor_.is_quiescent(conn_);
    }

    void on_data(BorrowedBytes cipher) noexcept override {
        while (!cipher.empty() && active()) {
            // The protocol is paused: park the rest of this receive until it resumes.
            const bool park = state_ == State::Open && !upper_receiving_;
            const std::size_t n = park ? cipher.size() : std::min(cipher.size(), kCipherPiece);
            ERR_clear_error();
            if (BIO_write(SSL_get_rbio(ssl_.get()), cipher.data(), static_cast<int>(n)) !=
                static_cast<int>(n)) {
                log_ssl_errors(shared_, reactor_.now(), "read buffer");
                fail(ENOMEM);
                return;
            }
            cipher = cipher.subspan(n);
            if (!park) {
                pump();
            }
        }
    }

    void on_writable() noexcept override {
        if (state_ == State::Open) {
            upper_.on_writable();
        }
    }

    void on_peer_eof() noexcept override {
        if (!active()) {
            return;
        }
        peer_eof_ = true;
        reactor_receiving_ = false;
        // From here an empty read BIO reads as end of stream instead of "wait for more".
        BIO_set_mem_eof_return(SSL_get_rbio(ssl_.get()), 0);
        if (state_ == State::Handshaking || upper_receiving_) {
            pump();
        }
    }

    void on_error(int err) noexcept override {
        if (active()) {
            fail(err);
        }
    }

    void on_timeout() noexcept override {
        timer_armed_ = false;
        switch (state_) {
        case State::Handshaking:
            fail(ETIMEDOUT);
            return;
        case State::Open:
            pump();
            return;
        case State::Failed:
            if (const int err = std::exchange(deferred_error_, 0); err != 0) {
                upper_.on_error(err);
            }
            return;
        case State::Closed:
            return;
        }
    }

private:
    enum class State : std::uint8_t { Handshaking, Open, Failed, Closed };

    [[nodiscard]] bool active() const noexcept {
        return state_ == State::Handshaking || state_ == State::Open;
    }

    [[nodiscard]] bool buffered() const noexcept {
        return SSL_has_pending(ssl_.get()) == 1 || BIO_ctrl_pending(SSL_get_rbio(ssl_.get())) > 0 ||
               (peer_eof_ && !eof_delivered_);
    }

    // The socket is read while the handshake needs bytes, and afterwards only while the
    // protocol wants them: stopping the reactor is what leaves the backlog in the kernel.
    void sync_receiving() noexcept {
        const bool want =
            state_ == State::Handshaking ||
            (state_ == State::Open && upper_receiving_ && !draining_ && !eof_delivered_);
        if (want == reactor_receiving_ || (want && peer_eof_)) {
            return;
        }
        reactor_receiving_ = want;
        if (want) {
            reactor_.start_receiving(conn_);
        } else {
            reactor_.stop_receiving(conn_);
        }
    }

    void pump() noexcept {
        if (state_ == State::Handshaking) {
            handshake();
        }
        if (state_ == State::Open) {
            read_records();
        }
        if (active()) {
            flush();
        }
    }

    void handshake() noexcept {
        ERR_clear_error();
        errno = 0;
        const int r = SSL_do_handshake(ssl_.get());
        if (r == 1) {
            state_ = State::Open;
            cancel_timer();
            end_handshake();
            sync_receiving();
            return;
        }
        on_ssl_error(SSL_get_error(ssl_.get(), r), "handshake failed");
    }

    void read_records() noexcept {
        std::array<std::byte, kRecordPlaintext> plain{};
        while (state_ == State::Open && upper_receiving_ && !eof_delivered_) {
            std::size_t n = 0;
            ERR_clear_error();
            errno = 0;
            if (SSL_read_ex(ssl_.get(), plain.data(), plain.size(), &n) != 1) {
                const int code = SSL_get_error(ssl_.get(), 0);
                // Everything parked is read; at most a partial record waits for the socket.
                if (code == SSL_ERROR_WANT_READ) {
                    draining_ = false;
                }
                on_ssl_error(code, "read failed");
                break;
            }
            upper_.on_data({plain.data(), n});
        }
        if (state_ == State::Open && BIO_ctrl_pending(SSL_get_rbio(ssl_.get())) == 0) {
            shrink_read_bio();
        }
        sync_receiving();
    }

    void on_ssl_error(int code, std::string_view what) noexcept {
        switch (code) {
        case SSL_ERROR_WANT_READ:
        case SSL_ERROR_WANT_WRITE:
            return;
        case SSL_ERROR_ZERO_RETURN:
            deliver_eof();
            return;
        case SSL_ERROR_SYSCALL:
            // Nothing queued and no errno: the read BIO ran dry after the peer's FIN.
            if (errno == 0 && ERR_peek_error() == 0) {
                deliver_eof();
                return;
            }
            break;
        default:
            break;
        }
        log_ssl_errors(shared_, reactor_.now(), what);
        // The alert OpenSSL queued tells the peer why, if it can still hear it.
        flush();
        fail(EPROTO);
    }

    void deliver_eof() noexcept {
        if (eof_delivered_) {
            return;
        }
        eof_delivered_ = true;
        if (state_ == State::Handshaking) {
            ++shared_.handshake_failures;
            state_ = State::Failed;
            cancel_timer();
            end_handshake();
        }
        sync_receiving();
        upper_.on_peer_eof();
    }

    void flush() noexcept {
        BIO* out = SSL_get_wbio(ssl_.get());
        while (BIO_ctrl_pending(out) > 0) {
            std::array<std::byte, kRecordPlaintext> buf{};
            const int n = BIO_read(out, buf.data(), static_cast<int>(buf.size()));
            if (n <= 0) {
                return;
            }
            reactor_.send(conn_, std::span(buf).first(static_cast<std::size_t>(n)));
        }
    }

    // Called only with the read BIO empty, so nothing it held is lost.
    void shrink_read_bio() noexcept {
        BUF_MEM* mem = nullptr;
        BIO_get_mem_ptr(SSL_get_rbio(ssl_.get()), &mem);
        if (mem == nullptr || mem->max <= kReadBioKeep) {
            return;
        }
        BIO* fresh = BIO_new(BIO_s_mem());
        if (fresh == nullptr) {
            return;
        }
        BIO_set_mem_eof_return(fresh, peer_eof_ ? 0 : -1);
        SSL_set0_rbio(ssl_.get(), fresh);
    }

    // Only on a reactor callback: the protocol hears of it before this returns.
    void fail(int err) noexcept {
        if (state_ == State::Handshaking) {
            ++shared_.handshake_failures;
        }
        state_ = State::Failed;
        cancel_timer();
        end_handshake();
        sync_receiving();
        upper_.on_error(err);
    }

    void defer_error(int err) noexcept {
        state_ = State::Failed;
        deferred_error_ = err;
        cancel_timer();
        sync_receiving();
        arm_timer(core::Millis{0});
    }

    void arm_timer(core::Millis delay) noexcept {
        timer_ = reactor_.arm_timer(delay, *this);
        timer_armed_ = true;
    }

    void cancel_timer() noexcept {
        if (timer_armed_) {
            reactor_.cancel_timer(timer_);
            timer_armed_ = false;
        }
    }

    void end_handshake() noexcept {
        if (counted_) {
            counted_ = false;
            --shared_.handshakes_in_flight;
        }
    }

    Shared& shared_;
    IReactor& reactor_;
    IStreamHandler& upper_;
    SslPtr ssl_;
    ConnId conn_;
    TimerId timer_;
    int deferred_error_ = 0;
    State state_ = State::Handshaking;
    bool timer_armed_ = false;
    bool counted_ = false;
    bool upper_receiving_ = false;
    bool reactor_receiving_ = false;
    // Records are parked below a protocol that has resumed: they go up before the socket is
    // read again.
    bool draining_ = false;
    bool peer_eof_ = false;
    bool eof_delivered_ = false;
    bool write_shut_ = false;
};

class TlsTransports final : public ITransportFactory, public IOffloadJob {
public:
    TlsTransports(IReactor& reactor, TlsFiles files, SslCtxPtr ctx) noexcept
        : reactor_(reactor), files_(std::move(files)), ctx_(std::move(ctx)) {}

    [[nodiscard]] std::expected<std::unique_ptr<ITransport>, int>
    attach(os::UniqueFd conn, IStreamHandler& handler) override {
        ERR_clear_error();
        SslPtr ssl{SSL_new(ctx_.get())};
        BIO* in = BIO_new(BIO_s_mem());
        BIO* out = BIO_new(BIO_s_mem());
        if (!ssl || in == nullptr || out == nullptr) {
            BIO_free(in);
            BIO_free(out);
            ERR_clear_error();
            return std::unexpected(ENOMEM);
        }
        // An empty BIO means "wait for more", not end of stream, until the reactor says so.
        BIO_set_mem_eof_return(in, -1);
        BIO_set_mem_eof_return(out, -1);
        SSL_set_bio(ssl.get(), in, out);
        auto transport = std::make_unique<TlsTransport>(shared_, reactor_, handler, std::move(ssl));
        const auto id = reactor_.attach(std::move(conn), *transport);
        if (!id) {
            return std::unexpected(id.error());
        }
        transport->start(*id);
        return transport;
    }

    void reload(OffloadPool& pool, IReloadHandler& done) override {
        pool_ = &pool;
        done_ = &done;
        if (loading_) {
            again_ = true;
            return;
        }
        loading_ = true;
        pool.submit(*this);
    }

    // On the pool: touches only the files, which never change, and its own result.
    void run() noexcept override { loaded_ = make_context(files_); }

    void complete() noexcept override {
        loading_ = false;
        std::expected<void, std::string> result;
        if (loaded_) {
            carry_ticket_keys(ctx_.get(), loaded_->get());
            // Each SSL holds a reference to the context it was made from, so connections
            // already open keep the old one until they close.
            ctx_ = std::move(*loaded_);
        } else {
            result = std::unexpected(std::move(loaded_.error()));
        }
        loaded_ = std::unexpected(std::string{});
        done_->on_reloaded(result);
        if (std::exchange(again_, false)) {
            loading_ = true;
            pool_->submit(*this);
        }
    }

    [[nodiscard]] std::size_t handshakes_in_flight() const noexcept override {
        return shared_.handshakes_in_flight;
    }
    [[nodiscard]] std::uint64_t handshake_failures() const noexcept override {
        return shared_.handshake_failures;
    }

private:
    IReactor& reactor_;
    const TlsFiles files_;
    SslCtxPtr ctx_;
    Shared shared_;
    OffloadPool* pool_ = nullptr;
    IReloadHandler* done_ = nullptr;
    std::expected<SslCtxPtr, std::string> loaded_ = std::unexpected(std::string{});
    bool loading_ = false;
    bool again_ = false;
};

} // namespace

std::expected<std::unique_ptr<ITransportFactory>, std::string>
make_tls_transports(IReactor& reactor, TlsFiles files) {
    auto ctx = make_context(files);
    if (!ctx) {
        return std::unexpected(std::move(ctx.error()));
    }
    return std::make_unique<TlsTransports>(reactor, std::move(files), std::move(*ctx));
}

} // namespace net
