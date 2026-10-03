#pragma once

#include "core/ports/clock.hpp"
#include "core/ports/live.hpp"
#include "infra/curl/multi.hpp"
#include "net/offload_pool.hpp"
#include "net/reactor.hpp"

#include <expected>
#include <memory>
#include <string>
#include <string_view>

namespace infra::packagers {

struct KubernetesConfig {
    // The API server as a pod reaches it.
    std::string api_url = "https://kubernetes.default.svc";
    // Where packager Jobs and their Secrets go: the gateway's own namespace.
    std::string namespace_name;
    // The pod's service account token, read again when a minute old: the kubelet rotates it.
    std::string token_file = "/var/run/secrets/kubernetes.io/serviceaccount/token";
    // The cluster's CA, which the API server's certificate chains to.
    std::string ca_file = "/var/run/secrets/kubernetes.io/serviceaccount/ca.crt";
    // The Job template's text (deploy/askedin/live-packager/job.yaml), with ${ULW_NAMESPACE},
    // ${ULW_IMAGE_TAG}, ${ULW_STREAM_ID} and ${ULW_STREAM_OWNER} to fill in and nothing else.
    std::string job_template;
    // The packager image's tag, best "<sha>@sha256:<digest>" (RUNBOOK 4a).
    std::string image_tag;
};

// The template's placeholders in the text; what fill_job_template() replaces.
[[nodiscard]] std::expected<void, std::string> check_job_template(std::string_view text);
// The template with the stream's values in. `owner` goes inside double quotes in the template:
// a user id's characters ([A-Za-z0-9._:@|+-]) never end a double-quoted YAML scalar.
[[nodiscard]] std::string fill_job_template(std::string_view text, std::string_view namespace_name,
                                            std::string_view image_tag, std::string_view stream,
                                            std::string_view owner);

// Packagers as Kubernetes Jobs, one per stream, made from the template (ADR-0083, ADR-0092)
// through the API server with the pod's service account, whose Role allows creating Jobs and
// Secrets and reading Jobs in its namespace, nothing else. Starting one is three calls: the
// stream's Secret (its SRT passphrase), the Job, then the Secret made the Job's dependent, so
// the Job's removal a day after it finishes takes the Secret with it. Each call that finds its
// object made already counts as done, so a start repeated after a lost answer finishes the
// first one's work. The Job's status says how the packager stands.
class KubernetesPackagers final : public core::ports::IPackagers {
public:
    class Impl;

    // Refuses a template without the placeholders, or with others, and a tag that could not
    // name an image.
    [[nodiscard]] static std::expected<std::unique_ptr<KubernetesPackagers>, std::string>
    create(net::IReactor& reactor, curl::Multi& multi, net::OffloadPool& offload,
           const core::ports::IClock& clock, KubernetesConfig config);

    explicit KubernetesPackagers(std::unique_ptr<Impl> impl) noexcept;
    // Calls in flight are cancelled; their callbacks never run.
    ~KubernetesPackagers() override;
    KubernetesPackagers(const KubernetesPackagers&) = delete;
    KubernetesPackagers& operator=(const KubernetesPackagers&) = delete;
    KubernetesPackagers(KubernetesPackagers&&) = delete;
    KubernetesPackagers& operator=(KubernetesPackagers&&) = delete;

    void start(const core::ports::PackagerSpec& spec, core::ports::PackagerDone done) override;
    void state(const core::LiveStreamId& stream, core::ports::PackagerStateDone done) override;

private:
    std::unique_ptr<Impl> impl_;
};

} // namespace infra::packagers
