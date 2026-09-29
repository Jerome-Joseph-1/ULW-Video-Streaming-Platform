# Images the sandbox pulls, pinned by digest; sourced by e2e-up.sh and e2e-down.sh.
#
# e2e-up.sh pulls each by digest and loads it into the kind node under its tag, which is what
# the manifests name: containerd on the node files an imported image under its tag only, so a
# digest reference would miss it and try the registry. The tag therefore always names the
# pinned content.

# Envoy Gateway v1.9.2's controller (its install.yaml names it by this tag) and the proxy it
# runs by default.
eg_image=docker.io/envoyproxy/gateway:v1.9.2
eg_digest=sha256:9d67017c442a70e6ae9f1a358e6c80eb653c67c2e40823d9a991e0daf37ba926
envoy_image=docker.io/envoyproxy/envoy:distroless-v1.39.1
envoy_digest=sha256:eb2c01c13125d1629637cb4e4cce7207009fb7cc2c8027f9742758549d15b6f4
# kube-router v2.10.0, the sandbox's NetworkPolicy enforcer (cluster/kube-router.yaml).
kube_router_image=docker.io/cloudnativelabs/kube-router:v2.10.0
kube_router_digest=sha256:0991f2cc7aaabe107b51c0c554d6b843f0483fd319b94f437fab638470c47c22
# metrics-server v0.9.0 (its components.yaml names this tag), installed by metrics-server.sh.
# The digest is the multi-platform index the release promotes to registry.k8s.io.
metrics_server_repo=registry.k8s.io/metrics-server/metrics-server
metrics_server_image=$metrics_server_repo:v0.9.0
metrics_server_digest=sha256:d9862115e7c7881280d3d75ca26bda8ffc0fc213315979575bf23ce9826205c0
# The same Postgres and MinIO builds as compose.yaml; these run beside the node, not in it.
pg_image=postgres:16@sha256:1a6ab3f5345eb6dbe04a1349529caabdb0ab09293a09590fad07b2246bfa4b54
minio_image=docker.io/pgsty/minio@sha256:b6bfe7239bfc83fb90d31612d9704d86039dd714f7904b3f1ad68f211e602372
# Built from this checkout.
built_images=(ulw/video-gateway:e2e ulw/video-worker:e2e ulw/mock-auth:e2e)
