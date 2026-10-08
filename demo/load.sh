#!/usr/bin/env bash
# Loads the platform's images from an archive the e2e workflow's demo job made (the
# demo-images-<sha> artifact of a run with demo_build), so up.sh runs them without pulling:
#
#   gh run download <run id> -n demo-images-<sha>     (or download it from the run's page)
#   demo/load.sh demo-images-<sha>.tar.gz
#   ULW_TAG=local-<sha> demo/up.sh
#
# The archive holds ghcr.io/jerome-joseph-1/ulw-{video-gateway,video-worker,chat,live-packager}
# tagged local-<sha>: the 12-digit commit they were built from. Check out that commit (git
# checkout <sha>) so the page and the browser MLS client match the images.
set -euo pipefail
archive=${1:-}
if [[ -z $archive ]]; then
    archive=$(ls -t demo-images-*.tar.gz "$(dirname "$0")"/../demo-images-*.tar.gz 2>/dev/null | head -1 || true)
fi
[[ -n $archive && -f $archive ]] || { echo "usage: demo/load.sh demo-images-<sha>.tar.gz" >&2; exit 2; }
command -v docker >/dev/null && docker info >/dev/null 2>&1 || { echo "load.sh: Docker is not running" >&2; exit 1; }
echo "==> Loading $archive ($(du -h "$archive" | cut -f1)); this takes a minute"
out=$(docker load -i "$archive")
echo "$out" | sed 's/^/    /'
tag=$(echo "$out" | sed -n 's/^Loaded image: .*:\(local-[0-9a-f]*\)$/\1/p' | head -1)
echo
echo "  Loaded. Start the demo with:   ULW_TAG=${tag:-local-<sha>} demo/up.sh"
