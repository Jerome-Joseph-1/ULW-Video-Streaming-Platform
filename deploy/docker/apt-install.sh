#!/bin/sh
# apt-install PACKAGE=VERSION...
#
# Installs exact versions from the Ubuntu archive as it stood at $UBUNTU_SNAPSHOT, so a rebuild
# months later installs the same bytes; apt checks each package against the signed Release file
# of that snapshot. snapshot.ubuntu.com is https only and the base image has no CA bundle, so a
# bootstrap ca-certificates comes from the image's own archive first (plain http, but signed).
# A build behind a TLS-inspecting proxy hands its CA in as the build secret `ca-bundle`.
set -eu
: "${UBUNTU_SNAPSHOT:?}"

if [ ! -e /etc/ssl/certs/ca-certificates.crt ]; then
    apt-get update -q
    apt-get install -qy --no-install-recommends ca-certificates
fi
ca=/etc/ssl/certs/ca-certificates.crt
if [ -r /run/secrets/ca-bundle ]; then
    ca=/run/secrets/ca-bundle
fi
apt() {
    # The snapshot service answers 503 now and then under load; a retry gets through.
    apt-get -o Acquire::https::CAInfo="$ca" -o Acquire::Retries=5 --snapshot "$UBUNTU_SNAPSHOT" "$@"
}
apt update -q
# Every package is pinned, so a downgrade can only be a pin undoing the bootstrap ca-certificates
# after the archive moved past the snapshot.
apt install -qy --no-install-recommends --allow-downgrades "$@"
apt-get clean
rm -rf /var/lib/apt/lists/*
