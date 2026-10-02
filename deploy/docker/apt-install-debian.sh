#!/bin/sh
# apt-install-debian PACKAGE=VERSION...
# apt-install-debian --policy PACKAGE...
#
# The Debian counterpart of apt-install.sh (docs/adr/0074): installs exact versions from the
# Debian archive and its security archive as they both stood at $DEBIAN_SNAPSHOT on
# snapshot.debian.org, so a rebuild months later installs the same bytes. Every package named
# must carry its version; what they depend on comes from the same snapshot, and so does every
# package the base image already holds (libc6, util-linux, perl-base and the rest), which are
# upgraded to the snapshot's versions before the install. --policy prints what the snapshot
# offers for each package instead of installing, which is how a bump finds the versions to pin.
#
# The image's own sources (deb.debian.org, the live archive) are replaced by the snapshot's,
# over https. The security and updates suites' Release files carry a Valid-Until a week or so
# after signing, which a snapshot is past by design, so apt's check of it is off (trixie's own
# Release has none); TLS to snapshot.debian.org is what keeps an older signed Release from
# being replayed on the way, and which archive state is installed is the pinned timestamp's
# choice. The base image has no CA bundle, so ca-certificates is bootstrapped from the live
# archive first (plain http, but signed, and fresh where the suite sets a Valid-Until), and
# the pinned install takes it, openssl, libssl3t64 and openssl-provider-legacy back to the
# snapshot's versions; the last step fails the build if any installed package is at a
# version the snapshot does not offer. A build behind a TLS-inspecting proxy hands its CA in
# as the build secret `ca-bundle`, which the snapshot's https is checked against instead.
set -eu
: "${DEBIAN_SNAPSHOT:?}"

policy=false
if [ "${1:-}" = --policy ]; then
    policy=true
    shift
fi
if ! $policy; then
    for spec in "$@"; do
        case $spec in
        *=*) ;;
        *)
            echo "apt-install-debian: $spec: every package must be pinned as NAME=VERSION" >&2
            exit 1
            ;;
        esac
    done
fi

ca=/etc/ssl/certs/ca-certificates.crt
if [ -r /run/secrets/ca-bundle ]; then
    ca=/run/secrets/ca-bundle
elif [ ! -e "$ca" ]; then
    if ! $policy; then
        # The bootstrap brings ca-certificates, openssl, libssl3t64 and the legacy provider
        # libssl3t64 depends on at whatever the live archive has; the pinned install below must
        # take all four back to the snapshot's (the check at the end fails the build if not).
        for pkg in ca-certificates openssl libssl3t64 openssl-provider-legacy; do
            case " $* " in
            *" $pkg="*) ;;
            *)
                echo "apt-install-debian: the first install in an image must pin" \
                    "ca-certificates, openssl, libssl3t64 and openssl-provider-legacy" >&2
                exit 1
                ;;
            esac
        done
    fi
    apt-get -o Acquire::Retries=10 update -q
    DEBIAN_FRONTEND=noninteractive apt-get install -qy --no-install-recommends ca-certificates
fi

# shellcheck disable=SC1091 # the image's, not the repository's
. /etc/os-release
suite=${VERSION_CODENAME:?}
keyring=/usr/share/keyrings/debian-archive-keyring.pgp
rm -f /etc/apt/sources.list /etc/apt/sources.list.d/debian.sources
cat >/etc/apt/sources.list.d/snapshot.sources <<SOURCES
Types: deb
URIs: https://snapshot.debian.org/archive/debian/$DEBIAN_SNAPSHOT/
Suites: $suite $suite-updates
Components: main
Signed-By: $keyring
Check-Valid-Until: no

Types: deb
URIs: https://snapshot.debian.org/archive/debian-security/$DEBIAN_SNAPSHOT/
Suites: $suite-security
Components: main
Signed-By: $keyring
Check-Valid-Until: no
SOURCES

apt() {
    # snapshot.debian.org throttles and answers 503 or resets now and then; a retry gets through.
    apt-get -o Acquire::https::CAInfo="$ca" -o Acquire::Retries=10 "$@"
}
apt update -q --error-on=any
if $policy; then
    apt-cache policy "$@"
    exit 0
fi
# The base image's own packages at the snapshot's versions: a pinned install upgrades only
# what it names. Nothing new is installed here.
DEBIAN_FRONTEND=noninteractive apt upgrade -qy --no-install-recommends --without-new-pkgs
# Every package is pinned, so a downgrade can only be a pin undoing the bootstrap's packages
# after the live archive moved past the snapshot.
DEBIAN_FRONTEND=noninteractive apt install -qy --no-install-recommends --allow-downgrades "$@"
# Every installed package at a version the snapshot offers: an upgrade never downgrades, so a
# package the bootstrap took past the snapshot and nothing pinned back would otherwise stay at
# the live archive's version, and a rebuild would install different bytes.
dpkg-query -W -f '${Package} ${Version}\n' | sort >/tmp/installed
# shellcheck disable=SC2046 # one argument per installed package
apt-cache madison $(cut -d' ' -f1 /tmp/installed) |
    awk -F'|' '{gsub(/ /, "", $1); gsub(/ /, "", $2); print $1 " " $2}' | sort -u >/tmp/offered
stray=$(comm -23 /tmp/installed /tmp/offered)
rm -f /tmp/installed /tmp/offered
if [ -n "$stray" ]; then
    echo "apt-install-debian: installed at versions snapshot $DEBIAN_SNAPSHOT does not offer:" >&2
    echo "$stray" >&2
    exit 1
fi
apt-get clean
rm -rf /var/lib/apt/lists/*
