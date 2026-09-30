#!/usr/bin/env bash
# Fetches the security scanners into tools/security/.tools, each pinned by version and SHA-256,
# and prints that directory (docs/adr/0072). A file whose hash does not match is deleted and
# the script fails; nothing unverified is unpacked or made executable. With names as
# arguments, fetches only those.
#
# Each SHA-256 is the release's own checksum file's line for the linux amd64 asset
# (osv-scanner_SHA256SUMS, trivy_<v>_checksums.txt, actionlint_<v>_checksums.txt); zizmor's is
# PyPI's for the manylinux x86_64 wheel, which carries the same static binary as its release.
set -euo pipefail

here=$(cd "$(dirname "$0")" && pwd)
tools=$here/.tools
mkdir -p "$tools"

# name  version  sha256  url
pins=(
    "osv-scanner 2.6.0 ca69b3d3cd08f889a49dc0a383122f71cc528b83803671df5fd874d97485b108
     https://github.com/google/osv-scanner/releases/download/v2.6.0/osv-scanner_linux_amd64"
    "trivy 0.74.0 2ae6fe3ee734b7fdf11335663e18c75ea12dccc76062f09f164a3b0f8be4371a
     https://github.com/aquasecurity/trivy/releases/download/v0.74.0/trivy_0.74.0_Linux-64bit.tar.gz"
    "actionlint 1.7.12 8aca8db96f1b94770f1b0d72b6dddcb1ebb8123cb3712530b08cc387b349a3d8
     https://github.com/rhysd/actionlint/releases/download/v1.7.12/actionlint_1.7.12_linux_amd64.tar.gz"
    "zizmor 1.30.1 eee12266b793cb87ad4a7e3af2e72404f8a63e3de5eb099b80bf7b1cfd232a8e
     https://files.pythonhosted.org/packages/63/55/1900b53d34dcebc207cf3ceb7957b3748e8bb40958f062e3853ab72f2395/zizmor-1.30.1-py3-none-manylinux_2_28_x86_64.whl"
)

only=" $* "

for pin in "${pins[@]}"; do
    read -r name version sum url <<<"${pin//$'\n'/ }"
    [[ $# -eq 0 || $only == *" $name "* ]] || continue
    # The stamp names the archive's hash, so a changed pin fetches again.
    if [[ -x $tools/$name && $(cat "$tools/$name.pin" 2>/dev/null) == "$version $sum" ]]; then
        continue
    fi
    echo "fetching $name $version" >&2
    archive=$tools/${url##*/}.part
    curl -fsSL --retry 5 --retry-all-errors --retry-delay 2 -o "$archive" "$url"
    if ! echo "$sum  $archive" | sha256sum --check --status; then
        rm -f "$archive"
        echo "$name $version: SHA-256 mismatch, expected $sum" >&2
        exit 1
    fi
    case $url in
        *.tar.gz) tar -xzf "$archive" -C "$tools" "$name" ;;
        *.whl) unzip -p "$archive" "zizmor-$version.data/scripts/zizmor" >"$tools/$name" ;;
        *) mv "$archive" "$tools/$name" ;;
    esac
    rm -f "$archive"
    chmod 0755 "$tools/$name"
    echo "$version $sum" >"$tools/$name.pin"
done

echo "$tools"
