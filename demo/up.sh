#!/usr/bin/env bash
# Starts the demo: every service on this machine, the page on http://localhost:8080.
#
#   demo/up.sh                  pull the published :main images and start
#   DEMO_BUILD=1 demo/up.sh     build the images from this checkout instead (20-40 minutes)
#   DEMO_LIVE=0 demo/up.sh      leave out the live recorder (egress, 1.5 GB to download)
#
# Other settings (defaults): DEMO_PORT (8080) for the page, DEMO_S3_PORT (9900) for the store,
# DEMO_RTC_TCP_PORT (7881) and DEMO_RTC_UDP_PORT (7882) for call and live media,
# DEMO_IMAGE_TAG (main) for which published build. Needs Docker with Compose v2 only.
set -euo pipefail
here=$(cd "$(dirname "$0")" && pwd)
cd "$here"

say() { printf '\033[1m==> %s\033[0m\n' "$*"; }
die() { printf 'up.sh: %s\n' "$*" >&2; exit 1; }

command -v docker >/dev/null || die "Docker is not installed (https://docs.docker.com/get-docker/)"
docker info >/dev/null 2>&1 || die "Docker is not running: start Docker Desktop (or the docker service) and try again"
docker compose version >/dev/null 2>&1 || die "Docker Compose v2 ('docker compose') is missing"

export DEMO_PORT=${DEMO_PORT:-8080}
export DEMO_S3_PORT=${DEMO_S3_PORT:-9900}
export DEMO_RTC_TCP_PORT=${DEMO_RTC_TCP_PORT:-7881}
export DEMO_RTC_UDP_PORT=${DEMO_RTC_UDP_PORT:-7882}
export DEMO_IMAGE_TAG=${DEMO_IMAGE_TAG:-main}
registry=ghcr.io/jerome-joseph-1

if [[ ${DEMO_BUILD:-0} == 1 ]]; then
    say "Building the images from this checkout ($(git -C .. rev-parse --short=12 HEAD 2>/dev/null || echo unknown))"
    sha=$(git -C .. rev-parse --short=12 HEAD 2>/dev/null || echo unknown)
    for target in gateway worker chat live-packager; do
        name=$target
        [[ $target == gateway || $target == worker ]] && name=video-$target
        say "  $target"
        docker build -f ../deploy/docker/Dockerfile --target "$target" \
            --build-arg ULW_GIT_SHA="$sha" -t "ulw-demo/ulw-$name:local" ..
    done
    export DEMO_GATEWAY_IMAGE=ulw-demo/ulw-video-gateway:local
    export DEMO_WORKER_IMAGE=ulw-demo/ulw-video-worker:local
    export DEMO_CHAT_IMAGE=ulw-demo/ulw-chat:local
    export DEMO_PACKAGER_IMAGE=ulw-demo/ulw-live-packager:local
else
    say "Pulling the platform's images ($registry/ulw-*:$DEMO_IMAGE_TAG)"
    for name in video-gateway video-worker chat live-packager; do
        docker pull -q "$registry/ulw-$name:$DEMO_IMAGE_TAG" >/dev/null ||
            die "could not pull $registry/ulw-$name:$DEMO_IMAGE_TAG (offline? try again, or DEMO_BUILD=1)"
    done
fi

services=()
if [[ ${DEMO_LIVE:-1} == 0 ]]; then
    services=(netns postgres minio auth migrate seed gateway worker chat redis livekit web)
    say "Leaving out the live recorder (DEMO_LIVE=0): going live will not record or play"
else
    say "Pulling the other images (the live recorder, egress, is about 1.5 GB the first time)"
fi
docker compose pull --quiet --ignore-buildable --policy missing ${services[@]+"${services[@]}"} 2>/dev/null ||
    docker compose pull --quiet --ignore-pull-failures ${services[@]+"${services[@]}"} 2>/dev/null || true

say "Assembling the gateway image (the gateway with the live packager beside it)"
docker compose build --quiet migrate

say "Starting"
docker compose up -d --remove-orphans ${services[@]+"${services[@]}"}

say "Waiting for every service to be ready (the first start takes a minute or two)"
deadline=$((SECONDS + 600))
# The web container's own check, run now rather than its last cached result.
ready() {
    docker compose exec -T web sh -c 'for u in /api/v1/readyz /_demo/chat/readyz /_demo/livekit/ /auth/healthz; do
        wget -q -O /dev/null "http://127.0.0.1:8080$u" || exit 1; done' >/dev/null 2>&1
}
until ready; do
    if ((SECONDS > deadline)); then
        docker compose ps -a
        die "not ready after 10 minutes; see 'docker compose -f demo/compose.yaml logs' and README.md, Troubleshooting"
    fi
    for svc in migrate seed; do
        state=$(docker compose ps -a --format '{{.State}} {{.ExitCode}}' "$svc" 2>/dev/null || true)
        if [[ $state == exited* && $state != *" 0" ]]; then
            docker compose logs --tail 40 "$svc"
            die "$svc failed"
        fi
    done
    printf '.'
    sleep 3
done
echo

cat <<MSG

  The demo is up:   http://localhost:$DEMO_PORT

  Open it in two browser windows (or a normal and a private one), and pick a different user
  in each: alice, bob or carol. Allow the camera and microphone when asked.
  Stop it with demo/down.sh (add --wipe to also forget videos and messages).

MSG
