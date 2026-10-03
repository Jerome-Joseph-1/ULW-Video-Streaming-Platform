#include "infra/packagers/kubernetes_packagers.hpp"

#include "core/util/json.hpp"

#include "later.hpp"

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstring>
#include <fstream>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace infra::packagers {

namespace {

using core::ports::PackagerError;
using core::ports::PackagerState;

constexpr std::string_view kNamespace = "${ULW_NAMESPACE}";
constexpr std::string_view kImageTag = "${ULW_IMAGE_TAG}";
constexpr std::string_view kStreamId = "${ULW_STREAM_ID}";
constexpr std::string_view kOwner = "${ULW_STREAM_OWNER}";
constexpr std::array kPlaceholders{kNamespace, kImageTag, kStreamId, kOwner};

// The API server answers from etcd in milliseconds; 10 s is a server or a path that is gone,
// and a caller waiting on a start can still report it while the owner waits.
constexpr core::Millis kRequestTimeout{10'000};
// A Job read back carries its managed fields; a few KiB, far under this.
constexpr std::size_t kMaxResponse = std::size_t{256} * 1024;
constexpr core::json::Limits kJsonLimits{.max_depth = 64, .max_bytes = kMaxResponse};
// A projected token is a JWT of about 1 KiB.
constexpr std::size_t kMaxToken = std::size_t{16} * 1024;
// The kubelet rotates the token well before it expires (at 80% of an hour by default).
constexpr core::Millis kTokenReuse{60'000};

constexpr int kOk = 200;
constexpr int kForbidden = 403;
constexpr int kNotFound = 404;
constexpr int kConflict = 409;
constexpr int kTooManyRequests = 429;
constexpr int kServerErrors = 500;

PackagerError classify(const curl::Result& result) noexcept {
    if (!result) {
        // A certificate that does not chain to the cluster CA stays refused.
        return result.error().kind == curl::FailureKind::Tls ||
                       result.error().kind == curl::FailureKind::Local
                   ? PackagerError::Refused
                   : PackagerError::Unavailable;
    }
    const int status = result->status;
    // The namespace's ResourceQuota spent: as many packagers as the platform takes are there
    // already, which the API server answers 403 Forbidden with "exceeded quota" in its Status
    // message (k8s.io/apiserver's quota admission). Any other 403 is the Role or the token.
    if (status == kForbidden && result->body.find("exceeded quota") != std::string::npos) {
        return PackagerError::Full;
    }
    // Unauthorized or forbidden is the Role or the token; a 4xx otherwise the template.
    return status == kTooManyRequests || status >= kServerErrors ? PackagerError::Unavailable
                                                                 : PackagerError::Refused;
}

bool ok(const curl::Result& result) noexcept {
    return result && result->status >= kOk && result->status < kOk + 100;
}

bool status_is(const curl::Result& result, int status) noexcept {
    return result && result->status == status;
}

std::string secret_name(std::string_view stream) {
    return "live-packager-" + std::string(stream);
}

std::optional<std::string> uid_of(std::string_view body) {
    const auto doc = core::json::parse(body, kJsonLimits);
    const core::json::Value* meta = doc ? doc->find("metadata") : nullptr;
    const core::json::Value* uid = meta != nullptr ? meta->find("uid") : nullptr;
    const auto text = uid != nullptr ? uid->as_string() : std::nullopt;
    if (!text || text->empty()) {
        return std::nullopt;
    }
    return std::string(*text);
}

std::optional<std::uint64_t> count_at(const core::json::Value* status, std::string_view key) {
    const core::json::Value* v = status != nullptr ? status->find(key) : nullptr;
    return v != nullptr ? v->as_u64() : std::nullopt;
}

// From the Job's status: its terminal conditions first, then its pods.
std::optional<PackagerState> state_of(std::string_view body) {
    const auto doc = core::json::parse(body, kJsonLimits);
    if (!doc) {
        return std::nullopt;
    }
    const core::json::Value* status = doc->find("status");
    const core::json::Value* conditions = status != nullptr ? status->find("conditions") : nullptr;
    const auto* list = conditions != nullptr ? conditions->as_array() : nullptr;
    if (list != nullptr) {
        for (const core::json::Value& c : *list) {
            const core::json::Value* type = c.find("type");
            const core::json::Value* value = c.find("status");
            if (type == nullptr || value == nullptr || value->as_string() != "True") {
                continue;
            }
            if (type->as_string() == "Complete") {
                return PackagerState::Finished;
            }
            if (type->as_string() == "Failed") {
                return PackagerState::Failed;
            }
        }
    }
    if (count_at(status, "succeeded").value_or(0) > 0) {
        return PackagerState::Finished;
    }
    const auto ready = count_at(status, "ready");
    // `ready` is counted from Kubernetes 1.24 on; before it, a running pod is the best sign.
    if (ready ? *ready > 0 : count_at(status, "active").value_or(0) > 0) {
        return PackagerState::Ready;
    }
    return PackagerState::Starting;
}

std::string read_token(const std::string& path) {
    std::ifstream in(path, std::ios::binary);
    std::string out;
    std::array<char, 4096> buf{};
    while (in && out.size() <= kMaxToken) {
        in.read(buf.data(), static_cast<std::streamsize>(buf.size()));
        out.append(buf.data(), static_cast<std::size_t>(in.gcount()));
    }
    if (out.size() > kMaxToken) {
        return {};
    }
    while (!out.empty() && (out.back() == '\n' || out.back() == '\r' || out.back() == ' ')) {
        out.pop_back();
    }
    // A header value: nothing in it may end the line.
    if (out.find_first_of("\r\n") != std::string::npos) {
        return {};
    }
    return out;
}

// The user id's own alphabet (core::UserId): nothing in it ends a double-quoted YAML scalar.
bool valid_owner(std::string_view owner) noexcept {
    return !owner.empty() && std::ranges::all_of(owner, [](char c) {
        return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') ||
               c == '.' || c == '_' || c == ':' || c == '@' || c == '|' || c == '+' || c == '-';
    });
}

} // namespace

std::expected<void, std::string> check_job_template(std::string_view text) {
    if (!text.contains(kStreamId)) {
        return std::unexpected("the job template names no ${ULW_STREAM_ID}");
    }
    for (std::size_t at = text.find("${"); at != std::string_view::npos;
         at = text.find("${", at + 2)) {
        const bool known = std::ranges::any_of(
            kPlaceholders, [&](std::string_view p) { return text.substr(at).starts_with(p); });
        if (!known) {
            const std::size_t end = text.find('}', at);
            return std::unexpected(
                "the job template has a placeholder this does not fill: " +
                std::string(text.substr(at, end == std::string_view::npos ? 2 : end - at + 1)));
        }
    }
    return {};
}

std::string fill_job_template(std::string_view text, std::string_view namespace_name,
                              std::string_view image_tag, std::string_view stream,
                              std::string_view owner) {
    std::string out;
    out.reserve(text.size() + 256);
    while (!text.empty()) {
        const std::size_t at = text.find("${");
        out.append(text.substr(0, at));
        if (at == std::string_view::npos) {
            break;
        }
        text.remove_prefix(at);
        if (text.starts_with(kNamespace)) {
            out.append(namespace_name);
            text.remove_prefix(kNamespace.size());
        } else if (text.starts_with(kImageTag)) {
            out.append(image_tag);
            text.remove_prefix(kImageTag.size());
        } else if (text.starts_with(kStreamId)) {
            out.append(stream);
            text.remove_prefix(kStreamId.size());
        } else if (text.starts_with(kOwner)) {
            out.append(owner);
            text.remove_prefix(kOwner.size());
        } else {
            out.append("${");
            text.remove_prefix(2);
        }
    }
    return out;
}

class KubernetesPackagers::Impl {
public:
    using Answer = curl::Result;
    using AnswerDone = std::move_only_function<void(Answer) noexcept>;
    using TokenDone =
        std::move_only_function<void(std::expected<std::string, PackagerError>) noexcept>;

    Impl(net::IReactor& reactor, curl::Multi& multi, net::OffloadPool& offload,
         const core::ports::IClock& clock, KubernetesConfig config) noexcept;
    ~Impl();
    Impl(const Impl&) = delete;
    Impl& operator=(const Impl&) = delete;

    void start(const core::ports::PackagerSpec& spec, core::ports::PackagerDone done) {
        const std::string stream = spec.stream.to_string();
        // A user id never holds what would end the template's quoted scalar; one that did is
        // refused here rather than written into a manifest (ADR-0092).
        if (!valid_owner(spec.owner.view())) {
            later_.post([done = std::move(done)]() mutable noexcept {
                done(std::unexpected(PackagerError::Refused));
            });
            return;
        }
        std::string job = fill_job_template(config_.job_template, config_.namespace_name,
                                            config_.image_tag, stream, spec.owner.view());
        create_job(stream, std::move(job), spec.passphrase, std::move(done));
    }

    void state(const core::LiveStreamId& stream, core::ports::PackagerStateDone done) {
        call(curl::Method::Get, job_path(stream.to_string()), {}, {},
             [done = std::move(done)](Answer got) mutable noexcept {
                 if (status_is(got, kNotFound)) {
                     done(PackagerState::Absent);
                     return;
                 }
                 if (!ok(got)) {
                     done(std::unexpected(classify(got)));
                     return;
                 }
                 const auto state = state_of(got->body);
                 if (!state) {
                     done(std::unexpected(PackagerError::Unavailable));
                     return;
                 }
                 done(*state);
             });
    }

    // Called by the token's offload job, back on the reactor thread.
    void token_read(std::string token) noexcept {
        reading_ = false;
        if (!token.empty()) {
            token_ = std::move(token);
            token_at_ = clock_.now();
        }
        std::vector<TokenDone> waiting = std::move(token_waiters_);
        token_waiters_.clear();
        for (TokenDone& done : waiting) {
            if (token_.empty()) {
                // No token is the pod's setup (automountServiceAccountToken), not an outage.
                done(std::unexpected(PackagerError::Refused));
            } else {
                done(token_);
            }
        }
    }

private:
    class Call;

    // The token file is read off the loop, on the offload pool. The job holds itself while it
    // is on the pool, so an owner destroyed meanwhile leaves the pool thread writing into a job
    // that is still alive; its completion then finds no owner and frees it. The path is the
    // job's own copy: the pool thread reads nothing of its owner.
    class TokenJob final : public net::IOffloadJob {
    public:
        TokenJob(Impl& owner, std::string path) noexcept : owner_(&owner), path_(std::move(path)) {}
        static void submit(net::OffloadPool& pool, const std::shared_ptr<TokenJob>& job) {
            job->self_ = job;
            pool.submit(*job);
        }
        // On the reactor thread, as complete() is.
        void detach() noexcept { owner_ = nullptr; }
        void run() noexcept override {
            try {
                token_ = read_token(path_);
            } catch (...) {
                token_.clear();
            }
        }
        void complete() noexcept override {
            // Last: this may be the job's last reference.
            const std::shared_ptr<TokenJob> keep = std::move(self_);
            if (owner_ != nullptr) {
                owner_->token_read(std::move(token_));
            }
        }

    private:
        Impl* owner_;
        std::string path_;
        std::string token_;
        std::shared_ptr<TokenJob> self_;
    };

    [[nodiscard]] std::string secrets_path() const {
        return "/api/v1/namespaces/" + config_.namespace_name + "/secrets";
    }
    [[nodiscard]] std::string jobs_path() const {
        return "/apis/batch/v1/namespaces/" + config_.namespace_name + "/jobs";
    }
    [[nodiscard]] std::string job_path(std::string_view stream) const {
        return jobs_path() + "/" + std::string(stream);
    }

    // The Job first, then its Secret made its dependent from the start, so the Job's removal (a
    // day after it finishes) removes the Secret too, and no Secret exists without its Job. The
    // pod waits for the Secret: the kubelet starts the container only once it can read it, and
    // the Secret is there before the image is pulled.
    void create_job(const std::string& stream, std::string job, std::string passphrase,
                    core::ports::PackagerDone done) {
        call(curl::Method::Post, jobs_path(), "application/yaml", std::move(job),
             [this, stream, passphrase = std::move(passphrase),
              done = std::move(done)](Answer made) mutable noexcept {
                 if (ok(made)) {
                     const auto uid = uid_of(made->body);
                     if (!uid) {
                         done(std::unexpected(PackagerError::Unavailable));
                         return;
                     }
                     create_secret(stream, *uid, passphrase, std::move(done));
                     return;
                 }
                 if (!status_is(made, kConflict)) {
                     done(std::unexpected(classify(made)));
                     return;
                 }
                 // Made before, by a start whose answer was lost: its uid is the one to name.
                 call(curl::Method::Get, job_path(stream), {}, {},
                      [this, stream, passphrase = std::move(passphrase),
                       done = std::move(done)](Answer got) mutable noexcept {
                          const auto uid = ok(got) ? uid_of(got->body) : std::nullopt;
                          if (!uid) {
                              done(std::unexpected(ok(got) ? PackagerError::Unavailable
                                                           : classify(got)));
                              return;
                          }
                          create_secret(stream, *uid, passphrase, std::move(done));
                      });
             });
    }

    void create_secret(const std::string& stream, const std::string& uid,
                       const std::string& passphrase, core::ports::PackagerDone done) {
        std::string secret = R"({"apiVersion":"v1","kind":"Secret","metadata":{"name":)";
        core::json::append_string(secret, secret_name(stream));
        secret += R"(,"labels":{"app.kubernetes.io/name":"live-packager",)"
                  R"("app.kubernetes.io/part-of":"ulw","app.kubernetes.io/instance":)";
        core::json::append_string(secret, stream);
        secret += R"(},"ownerReferences":[{"apiVersion":"batch/v1","kind":"Job","name":)";
        core::json::append_string(secret, stream);
        secret += R"(,"uid":)";
        core::json::append_string(secret, uid);
        secret += R"(}]},"type":"Opaque","stringData":{"ULW_LIVE_SRT_PASSPHRASE":)";
        core::json::append_string(secret, passphrase);
        secret += "}}";
        call(curl::Method::Post, secrets_path(), "application/json", std::move(secret),
             [done = std::move(done)](Answer made) mutable noexcept {
                 // Made before: by the same start, whose answer was lost.
                 if (!ok(made) && !status_is(made, kConflict)) {
                     done(std::unexpected(classify(made)));
                     return;
                 }
                 done({});
             });
    }

    void with_token(TokenDone done) {
        if (!token_.empty() && clock_.now() - token_at_ < kTokenReuse) {
            later_.post([this, done = std::move(done)]() mutable noexcept { done(token_); });
            return;
        }
        token_waiters_.push_back(std::move(done));
        if (!reading_) {
            reading_ = true;
            TokenJob::submit(offload_, token_job_);
        }
    }

    void call(curl::Method method, std::string path, std::string_view type, std::string body,
              AnswerDone done);
    void finished(Call& call, Answer answer) noexcept;

    curl::Multi& multi_;
    net::OffloadPool& offload_;
    const core::ports::IClock& clock_;
    KubernetesConfig config_;
    detail::Later later_;
    std::string token_;
    core::MonoTime token_at_;
    bool reading_ = false;
    std::vector<TokenDone> token_waiters_;
    std::shared_ptr<TokenJob> token_job_;
    std::vector<std::unique_ptr<Call>> calls_;
};

class KubernetesPackagers::Impl::Call final : public curl::ITransferHandler,
                                              public curl::IBodySource {
public:
    Call(Impl& owner, std::string body, AnswerDone done) noexcept
        : owner_(owner), body_(std::move(body)), done_(std::move(done)) {}
    ~Call() override = default;
    Call(const Call&) = delete;
    Call& operator=(const Call&) = delete;
    Call(Call&&) = delete;
    Call& operator=(Call&&) = delete;

    void start(std::unique_ptr<curl::Transfer> transfer) noexcept {
        transfer_ = std::move(transfer);
    }
    [[nodiscard]] std::uint64_t body_size() const noexcept { return body_.size(); }

    void on_transfer_done(curl::Result result) noexcept override {
        owner_.finished(*this, std::move(result));
    }

    std::size_t read_body(std::span<std::byte> out) noexcept override {
        const std::size_t n = std::min(out.size(), body_.size() - sent_);
        std::memcpy(out.data(), body_.data() + sent_, n);
        sent_ += n;
        return n;
    }

    [[nodiscard]] AnswerDone take_callback() noexcept { return std::move(done_); }

private:
    Impl& owner_;
    std::string body_;
    std::size_t sent_ = 0;
    AnswerDone done_;
    std::unique_ptr<curl::Transfer> transfer_;
};

// Out of line: the calls' type is complete only here.
KubernetesPackagers::Impl::Impl(net::IReactor& reactor, curl::Multi& multi,
                                net::OffloadPool& offload, const core::ports::IClock& clock,
                                KubernetesConfig config) noexcept
    : multi_(multi), offload_(offload), clock_(clock), config_(std::move(config)), later_(reactor),
      token_job_(std::make_shared<TokenJob>(*this, config_.token_file)) {
    while (config_.api_url.ends_with('/')) {
        config_.api_url.pop_back();
    }
}

KubernetesPackagers::Impl::~Impl() {
    token_job_->detach();
}

void KubernetesPackagers::Impl::call(curl::Method method, std::string path, std::string_view type,
                                     std::string body, AnswerDone done) {
    with_token([this, method, path = std::move(path), type = std::string(type),
                body = std::move(body), done = std::move(done)](
                   std::expected<std::string, PackagerError> token) mutable noexcept {
        if (!token) {
            // Local, so the classification reads it as the configuration's fault.
            done(std::unexpected(curl::Failure{.kind = curl::FailureKind::Local,
                                               .detail = "no service account token"}));
            return;
        }
        curl::Request request{
            .method = method,
            .url = config_.api_url + path,
            .headers = {"Authorization: Bearer " + *token, "Accept: application/json"},
            .max_body = kMaxResponse,
            .timeout = kRequestTimeout,
            .ca_file = config_.ca_file};
        if (!type.empty()) {
            request.headers.push_back("Content-Type: " + type);
        }
        auto& c =
            *calls_.emplace_back(std::make_unique<Call>(*this, std::move(body), std::move(done)));
        const bool sends = method == curl::Method::Post || method == curl::Method::Patch;
        auto transfer = sends ? curl::Transfer::start_upload(multi_, request, c.body_size(), c, c)
                              : curl::Transfer::start(multi_, request, c);
        if (!transfer) {
            later_.post([this, &c, failure = std::move(transfer.error())]() mutable noexcept {
                finished(c, std::unexpected(std::move(failure)));
            });
            return;
        }
        c.start(std::move(*transfer));
    });
}

void KubernetesPackagers::Impl::finished(Call& call, Answer answer) noexcept {
    AnswerDone done = call.take_callback();
    // Destroys the call and its transfer, which libcurl has already let go of.
    std::erase_if(calls_, [&](const auto& c) { return c.get() == &call; });
    done(std::move(answer));
}

std::expected<std::unique_ptr<KubernetesPackagers>, std::string>
KubernetesPackagers::create(net::IReactor& reactor, curl::Multi& multi, net::OffloadPool& offload,
                            const core::ports::IClock& clock, KubernetesConfig config) {
    if (!config.api_url.starts_with("https://") && !config.api_url.starts_with("http://")) {
        return std::unexpected("the API server's URL must be http:// or https://");
    }
    const auto label = [](std::string_view s) {
        return !s.empty() && s.size() <= 63 && s.front() != '-' && s.back() != '-' &&
               std::ranges::all_of(s, [](char c) {
                   return (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '-';
               });
    };
    if (!label(config.namespace_name)) {
        return std::unexpected("the namespace is not a DNS label");
    }
    const bool tag_ok = !config.image_tag.empty() && config.image_tag.size() <= 256 &&
                        std::ranges::all_of(config.image_tag, [](char c) {
                            return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
                                   (c >= '0' && c <= '9') || c == '.' || c == '_' || c == '-' ||
                                   c == ':' || c == '@';
                        });
    if (!tag_ok) {
        return std::unexpected("the image tag is not [A-Za-z0-9._:@-]");
    }
    if (auto checked = check_job_template(config.job_template); !checked) {
        return std::unexpected(std::move(checked.error()));
    }
    return std::make_unique<KubernetesPackagers>(
        std::make_unique<Impl>(reactor, multi, offload, clock, std::move(config)));
}

KubernetesPackagers::KubernetesPackagers(std::unique_ptr<Impl> impl) noexcept
    : impl_(std::move(impl)) {}

KubernetesPackagers::~KubernetesPackagers() = default;

void KubernetesPackagers::start(const core::ports::PackagerSpec& spec,
                                core::ports::PackagerDone done) {
    impl_->start(spec, std::move(done));
}

void KubernetesPackagers::state(const core::LiveStreamId& stream,
                                core::ports::PackagerStateDone done) {
    impl_->state(stream, std::move(done));
}

} // namespace infra::packagers
