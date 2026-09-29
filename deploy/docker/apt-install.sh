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
    # The bootstrap brings ca-certificates, openssl and libssl3t64 at whatever the live archive
    # has; the pinned install below must take all three back to the snapshot's versions. apt
    # downgrades only what is named, so libssl3t64 left unpinned stays at the live version and a
    # pinned libssl-dev, which wants its exact version, cannot be installed.
    for pkg in ca-certificates openssl libssl3t64; do
        case " $* " in
        *" $pkg="*) ;;
        *)
            echo "apt-install: the first install in an image must pin ca-certificates, openssl and libssl3t64" >&2
            exit 1
            ;;
        esac
    done
    apt-get update -q
    apt-get install -qy --no-install-recommends ca-certificates
fi
ca=/etc/ssl/certs/ca-certificates.crt
if [ -r /run/secrets/ca-bundle ]; then
    ca=/run/secrets/ca-bundle
fi
apt() {
    # The snapshot service answers 503 now and then under load; a retry gets through.
    apt-get -o Acquire::https::CAInfo="$ca" -o Acquire::Retries=10 --snapshot "$UBUNTU_SNAPSHOT" "$@"
}
apt update -q
# Every package is pinned, so a downgrade can only be a pin undoing the bootstrap's packages
# after the live archive moved past the snapshot.
apt install -qy --no-install-recommends --allow-downgrades "$@"
apt-get clean
rm -rf /var/lib/apt/lists/*
