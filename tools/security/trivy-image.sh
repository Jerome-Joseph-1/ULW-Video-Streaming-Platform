#!/usr/bin/env bash
# Usage: tools/security/trivy-image.sh IMAGE...
#
# Trivy's vulnerability scan of built images (docs/adr/0072): the Ubuntu packages and any
# language packages in them, against Trivy's advisory database as of the run (the database is
# data, updated every few hours, and deliberately not pinned; the scanner is). A HIGH or
# CRITICAL vulnerability that has a fixed package in the archive fails, unless
# tools/security/trivyignore.yaml lists it, unexpired. One with no fix yet is reported and does
# not fail: there is nothing to upgrade to, and the snapshot pin (deploy/docker/apt-install.sh)
# moves forward when one appears.
set -euo pipefail

if [[ $# -eq 0 ]]; then
    echo "usage: $0 IMAGE..." >&2
    exit 2
fi
root=$(cd "$(dirname "$0")/../.." && pwd)
trivy=$("$root/tools/security/tools.sh" trivy)/trivy
python3 "$root/tools/security/check-allowlists.py"
export TRIVY_CACHE_DIR=${TRIVY_CACHE_DIR:-$root/build/security/trivy-cache}

status=0
for image in "$@"; do
    echo "== $image"
    # Everything HIGH and above, fixed or not, for the log; then the gate.
    "$trivy" image --quiet --scanners vuln --severity HIGH,CRITICAL --format table \
        --ignorefile "$root/tools/security/trivyignore.yaml" "$image"
    "$trivy" image --quiet --skip-db-update --scanners vuln --severity HIGH,CRITICAL \
        --ignore-unfixed --exit-code 1 --format table \
        --ignorefile "$root/tools/security/trivyignore.yaml" "$image" >/dev/null || {
        echo "$image: a HIGH or CRITICAL vulnerability with a fix available (above)" >&2
        status=1
    }
done
exit $status
