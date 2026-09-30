#!/bin/sh
# apt-install-debian PACKAGE=VERSION...
# apt-install-debian --policy PACKAGE...
#
# The Debian counterpart of apt-install.sh (docs/adr/0071): installs exact versions from the
# Debian archive and its security archive as they both stood at $DEBIAN_SNAPSHOT on
# snapshot.debian.org, so a rebuild months later installs the same bytes. Every package named
# must carry its version; what they depend on comes from the same snapshot. --policy prints
# what the snapshot offers for each package instead of installing, which is how a bump finds
# the versions to pin.
#
# The image's own sources (deb.debian.org, the live archive) are replaced, not added to. The
# snapshot is plain http because the base image has no CA bundle to check https with; apt
# checks every index against the snapshot's Release file, signed by the archive keys the image
# ships, and every package against the index. Those Release files carry a Valid-Until a week or
# so after they were signed, which a snapshot is past by design, so that one check is off:
# which archive state is installed is the pinned timestamp's choice, not the mirror's.
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

# shellcheck disable=SC1091 # the image's, not the repository's
. /etc/os-release
suite=${VERSION_CODENAME:?}
keyring=/usr/share/keyrings/debian-archive-keyring.pgp
rm -f /etc/apt/sources.list /etc/apt/sources.list.d/debian.sources
cat >/etc/apt/sources.list.d/snapshot.sources <<EOF
Types: deb
URIs: http://snapshot.debian.org/archive/debian/$DEBIAN_SNAPSHOT/
Suites: $suite $suite-updates
Components: main
Signed-By: $keyring
Check-Valid-Until: no

Types: deb
URIs: http://snapshot.debian.org/archive/debian-security/$DEBIAN_SNAPSHOT/
Suites: $suite-security
Components: main
Signed-By: $keyring
Check-Valid-Until: no
EOF

apt() {
    # snapshot.debian.org throttles and answers 503 or resets now and then; a retry gets through.
    apt-get -o Acquire::Retries=10 "$@"
}
apt update -q
if $policy; then
    apt-cache policy "$@"
    exit 0
fi
DEBIAN_FRONTEND=noninteractive apt install -qy --no-install-recommends "$@"
apt-get clean
rm -rf /var/lib/apt/lists/*
