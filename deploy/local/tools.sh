#!/usr/bin/env bash
# Fetches the sandbox cluster's tools into deploy/local/.tools, each pinned by version and
# SHA-256. A file whose hash does not match is deleted and the script fails; nothing unverified
# is ever made executable. Prints the tools directory.
set -euo pipefail

here=$(cd "$(dirname "$0")" && pwd)
tools=$here/.tools
mkdir -p "$tools/schemas"

# name  version  sha256  url
pins=(
    "kind v0.33.0 aee6151561422756b764a4ae28e7f44cda5af5a9eead3cc9985112b1de8d8e0d
     https://github.com/kubernetes-sigs/kind/releases/download/v0.33.0/kind-linux-amd64"
    "kubectl v1.37.0 6129359f4e1f3848a5572ccb0b26cf28b8ca08cef38c95a765b2f64a2c961a2f
     https://dl.k8s.io/release/v1.37.0/bin/linux/amd64/kubectl"
    "kubeconform.tar.gz v0.8.0 9bc2bffbf71f261128533edaf912153948b7ff238f9a531ae6d34466ec287883
     https://github.com/yannh/kubeconform/releases/download/v0.8.0/kubeconform-linux-amd64.tar.gz"
    "woodpecker-cli.tar.gz v3.18.0 23e9b44eaa9dead25f39ad7ac69407f69769e1a8ce98795667bd3109c009ded7
     https://github.com/woodpecker-ci/woodpecker/releases/download/v3.18.0/woodpecker-cli_linux_amd64.tar.gz"
    "envoy-gateway.yaml v1.9.2 0412a72907e57ff9b73c56a7bf6df5190bf0f6e4f8bb4bba34e38630bbab5778
     https://github.com/envoyproxy/gateway/releases/download/v1.9.2/install.yaml"
    # metrics-server, for the load check's kubectl top (metrics-server.sh installs it).
    "metrics-server.yaml v0.9.0 1cec29a5267809306a2c6ec74a3e449abbb705b4a8beed0c8a1963910f72c79b
     https://github.com/kubernetes-sigs/metrics-server/releases/download/v0.9.0/components.yaml"
    # Docker's default seccomp profile, from which seccomp-profile.py derives the worker's.
    "moby-seccomp-default.json 85e237f 785b2429264afba4d594320337cb17f144f3c7d51585f9805eef72e28f4f9334
     https://raw.githubusercontent.com/moby/profiles/85e237f1fe229a0c61c9c7d8e743fa780d3b97ca/seccomp/default.json"
    # kubeconform's schemas for the built-in kinds the manifests use, Kubernetes v1.37.0, from
    # yannh/kubernetes-json-schema at a fixed commit. The CRD kinds' schemas are generated
    # from the CRDs in envoy-gateway.yaml (crd-schemas.py).
    "schemas/serviceaccount-v1.json a6f9a32 8193d6c3561475c6d3d5c44e1faedb1df53905373d904bc17015694326d659cf
     https://raw.githubusercontent.com/yannh/kubernetes-json-schema/a6f9a32d2ccb64b6e4f5b41419b9c2e8ee0cce18/v1.37.0-standalone-strict/serviceaccount-v1.json"
    "schemas/clusterrole-rbac-v1.json a6f9a32 3fd79fbc322d89090be016a046630b6a399941547e7fbf7501645ee379e4fccb
     https://raw.githubusercontent.com/yannh/kubernetes-json-schema/a6f9a32d2ccb64b6e4f5b41419b9c2e8ee0cce18/v1.37.0-standalone-strict/clusterrole-rbac-v1.json"
    "schemas/clusterrolebinding-rbac-v1.json a6f9a32 3f83a2198fe9c178bf742849df9a3fb719fe2f3bb8ef425fee855931dad554cf
     https://raw.githubusercontent.com/yannh/kubernetes-json-schema/a6f9a32d2ccb64b6e4f5b41419b9c2e8ee0cce18/v1.37.0-standalone-strict/clusterrolebinding-rbac-v1.json"
    "schemas/daemonset-apps-v1.json a6f9a32 410871aa51ca678b861fddf530925f1fc6c9f38766975cbae3bb113c339fcecc
     https://raw.githubusercontent.com/yannh/kubernetes-json-schema/a6f9a32d2ccb64b6e4f5b41419b9c2e8ee0cce18/v1.37.0-standalone-strict/daemonset-apps-v1.json"
    "schemas/deployment-apps-v1.json a6f9a32 0b64451c0b8c36ea06dfebf952718810ae24a07779fb0ed2a6b03f1cc8735a54
     https://raw.githubusercontent.com/yannh/kubernetes-json-schema/a6f9a32d2ccb64b6e4f5b41419b9c2e8ee0cce18/v1.37.0-standalone-strict/deployment-apps-v1.json"
    "schemas/service-v1.json a6f9a32 8bf019854daed511e7c174896a898173fa65d88ec5937c687a37303d4cc9351b
     https://raw.githubusercontent.com/yannh/kubernetes-json-schema/a6f9a32d2ccb64b6e4f5b41419b9c2e8ee0cce18/v1.37.0-standalone-strict/service-v1.json"
    "schemas/namespace-v1.json a6f9a32 324fae677b98d1a6d54340db0c334d053e8ffbafceb3f73326e41de2610d5843
     https://raw.githubusercontent.com/yannh/kubernetes-json-schema/a6f9a32d2ccb64b6e4f5b41419b9c2e8ee0cce18/v1.37.0-standalone-strict/namespace-v1.json"
    "schemas/networkpolicy-networking-v1.json a6f9a32 f6324cc464f62228b0418f438d167208e4f86c7e3677ba30f608e79a8b26ba79
     https://raw.githubusercontent.com/yannh/kubernetes-json-schema/a6f9a32d2ccb64b6e4f5b41419b9c2e8ee0cce18/v1.37.0-standalone-strict/networkpolicy-networking-v1.json"
)

verified() {
    [[ -f $1 ]] && echo "$2  $1" | sha256sum --check --status
}

for pin in "${pins[@]}"; do
    read -r name version sum url <<<"${pin//$'\n'/ }"
    file=$tools/$name
    if verified "$file" "$sum"; then
        continue
    fi
    echo "fetching $name $version" >&2
    curl -fsSL --retry 3 -o "$file.part" "$url"
    if ! verified "$file.part" "$sum"; then
        rm -f "$file.part"
        echo "$name $version: SHA-256 mismatch, expected $sum" >&2
        exit 1
    fi
    mv "$file.part" "$file"
done

chmod 0755 "$tools/kind" "$tools/kubectl"
for tool in kubeconform woodpecker-cli; do
    if [[ ! -x $tools/$tool || $tools/$tool -ot $tools/$tool.tar.gz ]]; then
        tar -xzmf "$tools/$tool.tar.gz" -C "$tools" "$tool"
    fi
done
echo "$tools"
