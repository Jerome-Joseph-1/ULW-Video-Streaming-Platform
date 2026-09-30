#!/usr/bin/env bash
# Checks every dependency this repository pins against OSV (docs/adr/0072): the Cargo, npm
# and hash-pinned pip lock files, and the vendored C and C++ tarballs of
# cmake/Dependencies.cmake (tools/security/cpp-deps.py). Any advisory not in
# tools/security/osv-scanner.toml, or past its ignoreUntil there, fails.
#
# Extra arguments go to osv-scanner, e.g. --offline-vulnerabilities --download-offline-databases
# where api.osv.dev cannot be reached (the C and C++ commits need the API).
set -euo pipefail

root=$(cd "$(dirname "$0")/../.." && pwd)
cd "$root"
tools=$(tools/security/tools.sh osv-scanner)
python3 tools/security/check-allowlists.py

work=$(mktemp -d)
trap 'rm -rf "$work"' EXIT
python3 tools/security/cpp-deps.py "$work/cpp-deps.json"

lockfiles=(-L "osv-scanner:$work/cpp-deps.json")
while IFS= read -r file; do
    lockfiles+=(-L "$file")
done < <(git ls-files '*Cargo.lock' '*package-lock.json')
# Only requirement files that pin by hash are lock files; anything else would be scanned for
# whatever version it happens to name.
while IFS= read -r file; do
    if grep -q -- '--hash=sha256:' "$file"; then
        lockfiles+=(-L "requirements.txt:$file")
    else
        echo "osv-scan: $file pins no hashes; not a lock file, not scanned" >&2
        exit 1
    fi
done < <(git ls-files '*requirements*.txt')

"$tools/osv-scanner" scan source --config tools/security/osv-scanner.toml "$@" "${lockfiles[@]}"
