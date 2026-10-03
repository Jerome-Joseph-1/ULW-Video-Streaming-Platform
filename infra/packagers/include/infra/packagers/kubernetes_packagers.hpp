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

// What fills the Job template besides the stream (deploy/kubernetes/live-packager/job.yaml):
// each field is the config.env key of the same name, which a by-hand start (RUNBOOK.md, step 9)
// sources and envsubst fills in, so the gateway and an operator make the same Job.
struct JobValues {
    // ${LIVE_NAMESPACE}: where packager Jobs and their Secrets go, the packagers' own namespace.
    std::string namespace_name;
    // ${LIVE_PACKAGER_IMAGE_TAG}: the packager image's tag, best "<sha>@sha256:<digest>".
    std::string image_tag;
    // ${IMAGE_PULL_POLICY}: Always, IfNotPresent or Never.
    std::string pull_policy = "IfNotPresent";
    // ${STORAGE}, ${R2_ACCOUNT_ID}, ${S3_ENDPOINT}, ${BUCKET}: the object store, as the gateway's
    // own; r2 or minio (a packager on the cluster cannot reach a gateway's directory).
    std::string storage;
    std::string r2_account_id;
    std::string s3_endpoint;
    std::string bucket;
    // ${LIVE_PACKAGER_SECRET}: the Secret with the packager's database URL and store keys.
    std::string packager_secret = "live-packager-secrets";
};

struct KubernetesConfig {
    // The API server as a pod reaches it.
    std::string api_url = "https://kubernetes.default.svc";
    // The pod's service account token, read again when a minute old: the kubelet rotates it.
    std::string token_file = "/var/run/secrets/kubernetes.io/serviceaccount/token";
    // The cluster's CA, which the API server's certificate chains to.
    std::string ca_file = "/var/run/secrets/kubernetes.io/serviceaccount/ca.crt";
    // The Job template's text, with JobValues' placeholders, ${ULW_STREAM_ID} and
    // ${ULW_STREAM_OWNER} to fill in and nothing else.
    std::string job_template;
    JobValues job;
};

// The template's placeholders in the text; what fill_job_template() replaces.
[[nodiscard]] std::expected<void, std::string> check_job_template(std::string_view text);
// The values a Job may be filled with: a DNS label for the namespace, and nothing in any value
// that would end the YAML scalar the template puts it in.
[[nodiscard]] std::expected<void, std::string> check_job_values(const JobValues& values);
// The template with the stream's values in. `owner` goes inside double quotes in the template:
// a user id's characters ([A-Za-z0-9._:@|+-]) never end a double-quoted YAML scalar.
[[nodiscard]] std::string fill_job_template(std::string_view text, const JobValues& values,
                                            std::string_view stream, std::string_view owner);

// Packagers as Kubernetes Jobs, one per stream, made from the template (ADR-0083, ADR-0092)
// through the API server with the gateway's service account, whose Role in the packagers' own
// namespace allows creating Jobs and Secrets and reading Jobs, nothing else; an admission policy
// holds what it creates to the packager's shape. Starting one is two calls: the Job, then the
// stream's Secret (its SRT passphrase) created with the Job as its owner, so the Job's removal a
// day after it finishes takes the Secret with it and no Secret outlives a Job that failed to
// start. A call that finds its object made already counts as done, so a start repeated after a
// lost answer finishes the first one's work. The Job's status says how the packager stands.
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
