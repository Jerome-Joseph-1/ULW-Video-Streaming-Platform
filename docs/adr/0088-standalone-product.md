# 0088. ULW is a standalone product; operators configure it, nothing assumes one of them

Status: Accepted
Date: 2026-10-03
Amends: ADR-0018 (the identity provider is the operator's: no default audience against a JWKS,
the subject claim configurable, no fallback to `id`); ADR-0031, ADR-0032, ADR-0037, ADR-0052,
ADR-0053, ADR-0063, ADR-0066, ADR-0078, ADR-0082 and ADR-0083 (their `deploy/askedin` paths,
overlays and runbook steps are now `deploy/kubernetes`'s, configured per environment by its
`config.env`); ADR-0072's and ADR-0085's image pin rule (it checks the rendered production
overlay, not files under `overlays/prod/`); ADR-0084 (the TURN port is `TURN_PORT`, an
operator's choice; 3479 stays the example for two environments on one address)

## Context

ULW was built to run inside one company's platform, Askedin's, and it said so throughout:
identity was "borrowed from Askedin" (ADR-0018) with `JWT_AUDIENCE` defaulting to
`askedin-platform` and a fallback to Askedin's own `id` claim when a token had no `sub`; the
Kubernetes manifests were `deploy/askedin/overlays/{stage,prod}`, plain files with Askedin's
namespaces (`apps-stage`, `apps`), Gateway (`askedin-gateway`), hostnames (`stage.askedin.com`,
`askedin.com`), JWKS URLs and issuer written into them, meant to be copied into Askedin's
monorepo and synced by its ArgoCD; `deploy/askedin/woodpecker.yml` was Askedin's pipeline; the
runbook was written for the person with access to Askedin's single-node cluster; and the
integration guide addressed Askedin's teams.

The owner has decided that ULW is a product of its own. Askedin becomes one operator among any:
it pulls the public images (ADR-0085) and sets its own configuration, as anyone else would.
Nothing in this repository may then carry an operator's names, hosts, defaults or tooling, and
everything an operator must choose has to be a setting, in one place per environment, documented.

## Options

| Option | For | Against |
|---|---|---|
| Keep `deploy/askedin` and add a generic tree beside it | No change for Askedin | Two trees to keep in step, and the product still names one customer in its defaults and code |
| Plain manifests with placeholders the operator edits in place (`sed`, by hand) | No tooling beyond kubectl | Values scattered over a dozen files per environment; every upgrade is a merge of edited copies |
| A Helm chart with a values file | Familiar to many operators | A second templating language over manifests the repository validates as plain YAML; the sandbox and CI would render through it too |
| A kustomize base, one `config.env` per environment, and a component that copies each value into the manifests (replacements) | One file holds every operator-specific value; the base stays plain, valid YAML that kubeconform and Trivy check as written; `kubectl apply -k` needs nothing else; the sandbox is just another operator of the same base; a missing key fails the build | Replacements are verbose, and a value used inside a string (the TURN port in LiveKit's configuration) needs an env var to carry it |

## Decision

- **Identity.** `JWT_AUDIENCE` has no default when `JWKS_URL` is set: the gateway and chat
  refuse to start (exit 2) without it, naming the variable. With a local development key set
  (`ULW_DEV_JWKS_FILE`, `ULW_DEV_MODE=1`) it defaults to `ulw-dev`, which `ulw_devtoken` now
  mints by default. `JWT_ISSUER` and `JWKS_URL` stay required as before. The user is the claim
  `ULW_JWT_SUBJECT_CLAIM` names (default `sub`; 1 to 64 of `A-Z a-z 0-9 _ . : / -`); no other
  claim stands in for it. `sub` must be a string; another claim may also be a non-negative
  integer, taken in decimal, as the `id` fallback took it. Both settings are read by one function
  (`ops::token_rules`) for the gateway and chat, and logged with the gateway's effective
  configuration.
- **Deployment.** `deploy/askedin` is replaced by `deploy/kubernetes`: `base/` (every manifest,
  one directory per part, with example values: namespace `ulw`, `video.example.com`,
  `id.example.com`), `components/operator-config/` (the replacements), `overlays/staging` (follows
  `main`) and `overlays/production` (pinned, a larger worker and two live streams), each with its
  `config.env`; `cluster/` for what is installed once per cluster (STUNner's operator, dataplane
  and class; the worker's seccomp profile); and the packager's Job template, now filled from the
  same `config.env`. `config.env` holds namespace, Gateway, hostname, pod network, identity
  provider, cookie and origins, object store (backend, account id or endpoint, bucket), the four
  image tags and pull policy, TURN port and the five Secret names; secrets stay in Secrets.
- **What moved where.** `overlays/stage/<part>/` became `base/<part>/`; `overlays/prod/` became
  `overlays/production` (its differences are config values and two patches); `stunner/` and
  `seccomp/` became `cluster/stunner/` and `cluster/seccomp/`; `live-packager/job.yaml` and
  `RUNBOOK.md` moved to `deploy/kubernetes/`; `woodpecker.yml` and the woodpecker-cli lint are
  gone. The R2 account id and bucket moved from the Secrets to `config.env`; the JWT issuer is
  never read from a Secret.
- **The image pin rule** (ADR-0072, ADR-0085) is kept and retargeted: `check-image-pins.py`
  takes `--kubernetes` (the tree as written and the staging overlay as rendered: own builds by
  any tag) and `--pinned` (the production overlay and its packager Job as rendered: own builds
  only by a 40-hex SHA or a digest). The `:<sha>` placeholder is gone; the example production
  `config.env` names a placeholder SHA.
- **The sandbox** (`deploy/local`, `deploy/stunner`) renders the same base parts through the same
  component from `deploy/local/config.env`, in namespace `ulw` behind Gateway `public-gateway` in
  `gateway-system`; its identity-header stand-in is `edge-identity.yaml`.
- **Documentation.** The runbook is an operator's guide; `docs/integration/operations-contract.md`
  is `operator-contract.md`; the integration guide addresses whoever integrates a deployment.
  Earlier ADRs are not rewritten: their Askedin-specific parts read as amended here.

## Consequences

- An operator copies an overlay, fills in one file, creates its Secrets and applies; nothing
  else in the repository changes per operator. Askedin's own overlays, hosts, issuer and
  rotation procedure live in its own repository.
- A deployment that relied on the old defaults breaks loudly, not silently: without
  `JWT_AUDIENCE` the services refuse to start, and tokens with `id` but no `sub` are refused
  until `ULW_JWT_SUBJECT_CLAIM=id` is set (docs/integration/changelog.md).
- A key missing from `config.env`, or a manifest field a replacement expects, fails
  `kubectl kustomize`; the base's example values never reach a cluster unnoticed.
- What `config.env` does not cover (replicas, resources, limits, extra hostnames, a proxy hop
  count, another Prometheus namespace, Envoy's data-plane labels) is changed by kustomize
  patches in the operator's overlay, which the runbook names where each comes up.
- The sandbox cluster runs the new layout end to end only where Docker and kind can
  (`make e2e-up`, `make e2e-test`, `make validate-manifests`); the e2e workflow runs them
  nightly.
- Reopen if operators need settings per service that one `config.env` per environment cannot
  express, or a packaging (Helm, an operator) that kustomize does not serve.
