#!/usr/bin/env bash
# Lints .github/ (docs/adr/0072): actionlint for the workflows' syntax, expressions and the
# shell in their run steps (with shellcheck, when it is installed), and zizmor for their
# security: permissions, template injection, credentials left in the checkout, cache
# poisoning, unpinned actions. Any finding fails. zizmor runs offline, so the audits that
# would ask GitHub's API about the pinned actions are skipped; its configuration is
# .github/zizmor.yml.
set -euo pipefail

root=$(cd "$(dirname "$0")/../.." && pwd)
cd "$root"
tools=$(tools/security/tools.sh actionlint zizmor)

"$tools/actionlint" -no-color
"$tools/zizmor" --offline --no-progress --config .github/zizmor.yml .github
