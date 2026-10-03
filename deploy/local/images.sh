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
# STUNner v1.2.1 (deploy/stunner/up.sh): the gateway operator and the stunnerd dataplane it
# runs. The digests are the multi-platform indexes Docker Hub serves for the release tags, and
# match what deploy/kubernetes/cluster/stunner names.
stunner_operator_image=docker.io/l7mp/stunner-gateway-operator:1.2.1
stunner_operator_digest=sha256:0ea40a1d42d50a382803a6f182994e61ce6027673133bdeee970874610558516
stunnerd_image=docker.io/l7mp/stunnerd:1.2.1
stunnerd_digest=sha256:a9a06157564d0630a8a95b9eab1307340bc1e710c53c890daff5fd0dfee50617
# LiveKit v1.13.7, the SFU behind STUNner (ADR-0020). The local call suite runs the same digest
# outside the cluster, so e2e-down.sh removes only the tag and leaves the content.
livekit_image=docker.io/livekit/livekit-server:v1.13.7
livekit_digest=sha256:6fd3b7088874c4d119160dd688798dfec852bc014786d392caad15f6f63912a3
# Redis 7.4.6 (alpine), LiveKit's bus to egress, as compose.yaml's calls profile runs it and
# deploy/kubernetes/base/livekit-redis names it.
livekit_redis_image=docker.io/library/redis:7.4.6-alpine
livekit_redis_digest=sha256:3b73847e72874be07e6657b129a94761662b79bc0f679273757d4218573b2a98
# The client of tests/cluster/stunner_check.py, which runs its stdlib-only probe from the
# sandbox's outside network (sandbox.sh). Python 3.13.13.
probe_image=docker.io/library/python@sha256:e81548ac35b07a3bd4805f275107592ef458b1e893c0e04d45aedaa19416cca5
# The same Postgres and MinIO builds as compose.yaml; these run beside the node, not in it.
pg_image=postgres:16@sha256:1a6ab3f5345eb6dbe04a1349529caabdb0ab09293a09590fad07b2246bfa4b54
minio_image=docker.io/pgsty/minio@sha256:b6bfe7239bfc83fb90d31612d9704d86039dd714f7904b3f1ad68f211e602372
# Built from this checkout. live-packager is built, checked and scanned like the rest but runs no
# pod: a packager is one Job per stream, started for a stream (deploy/kubernetes/live-packager).
built_images=(ulw/video-gateway:e2e ulw/video-worker:e2e ulw/mock-auth:e2e ulw/chat:e2e
    ulw/live-packager:e2e)
