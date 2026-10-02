#!/usr/bin/env bash
# Usage: tools/security/trivy-image.sh IMAGE...
#
# Trivy's vulnerability scan of built images (docs/adr/0072): the Ubuntu packages and any
# language packages in them, against Trivy's advisory database as of the run (the database is
# data, updated every few hours, and deliberately not pinned; the scanner is).
#
# Two passes over each image:
#   report  everything, unfiltered, as JSON in $TRIVY_REPORT_DIR, and a table of every HIGH or
#           CRITICAL (by Trivy's rating or NVD's), fixed or not, allowlisted or not, in the job
#           summary (tools/security/trivy-report.py). Never fails.
#   gate    fails on a vulnerability Trivy rates HIGH or CRITICAL that has a fixed version in
#           the archive, unless tools/security/trivyignore.yaml lists it, unexpired.
# The report pass exists because the gate alone would hide the ones that matter most here:
# Trivy rates Ubuntu packages by Ubuntu's priority, which puts ffmpeg's CVEs at medium where
# NVD says high or critical, and records a fix only in Ubuntu Pro's ESM archive as no fix.
set -euo pipefail

if [[ $# -eq 0 ]]; then
    echo "usage: $0 IMAGE..." >&2
    exit 2
fi
root=$(cd "$(dirname "$0")/../.." && pwd)
trivy=$("$root/tools/security/tools.sh" trivy)/trivy
python3 "$root/tools/security/check-allowlists.py"
export TRIVY_CACHE_DIR=${TRIVY_CACHE_DIR:-$root/build/security/trivy-cache}
reports=${TRIVY_REPORT_DIR:-$root/build/security/trivy-image}
ignorefile=$root/tools/security/trivyignore.yaml
mkdir -p "$reports"

jsons=()
for image in "$@"; do
    json=$reports/${image//[:\/@]/_}.json
    "$trivy" image --quiet --scanners vuln --format json --output "$json" "$image"
    jsons+=("$json")
done
python3 "$root/tools/security/trivy-report.py" "$ignorefile" "${jsons[@]}" |
    tee "$reports/summary.md"

status=0
for image in "$@"; do
    echo "== gate: $image"
    "$trivy" image --quiet --skip-db-update --scanners vuln --severity HIGH,CRITICAL \
        --ignore-unfixed --ignorefile "$ignorefile" --exit-code 1 --format table "$image" || {
        echo "$image: a HIGH or CRITICAL vulnerability with a fix available (above)" >&2
        status=1
    }
done
exit $status
