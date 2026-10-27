#!/usr/bin/env bash
# Configures and builds a preset (default dev), as README.md's build does:
#
#   tools/dev/build.sh            build/dev, every target
#   tools/dev/build.sh ci         build/ci, warnings as errors, as the pull request lane
#   tools/dev/build.sh dev gateway_server chat_server     only those targets
set -euo pipefail
root=$(cd "$(dirname "$0")/../.." && pwd)
cd "$root"
preset=${1:-dev}
shift $(($# > 0 ? 1 : 0))
cmake --preset "$preset"
if (($#)); then
    cmake --build --preset "$preset" --target "$@"
else
    cmake --build --preset "$preset"
fi
