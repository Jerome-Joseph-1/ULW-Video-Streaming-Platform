#!/usr/bin/env bash
# Lints .github/ (docs/adr/0072): actionlint for the workflows' syntax, expressions and the
# shell in their run steps (with shellcheck, when it is installed), and zizmor for their
# security: permissions, template injection, credentials left in the checkout, cache
# poisoning, unpinned actions. Any finding fails. Its configuration is .github/zizmor.yml.
#
# With GH_TOKEN set (CI passes the job's own read-only token), zizmor also runs its online
# audits, which ask GitHub's API about each pinned action: impostor-commit (the SHA is really
# in that repository, not a fork's), ref-version-mismatch (the SHA is the tag the comment
# names) and known-vulnerable-actions. Without it, as on a developer's machine, it runs
# offline and those three are skipped.
set -euo pipefail

root=$(cd "$(dirname "$0")/../.." && pwd)
cd "$root"
tools=$(tools/security/tools.sh actionlint zizmor)

"$tools/actionlint" -no-color
if [[ -n ${GH_TOKEN:-} ]]; then
    mode=()
else
    echo "lint-workflows: GH_TOKEN is not set; zizmor's online audits are skipped" >&2
    mode=(--offline)
fi
"$tools/zizmor" "${mode[@]}" --no-progress --config .github/zizmor.yml .github
