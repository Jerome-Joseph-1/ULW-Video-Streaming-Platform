# shellcheck shell=bash disable=SC2034  # sourced: the variables are its callers'
# Shared by the tools/dev scripts; sourced, never run. Settles where things are and which
# backing services this run uses (docs/development/cloud-dev.md).
#
#   ULW_DEV_BACKEND   local (default) or cloud: the default for the two below
#   ULW_DEV_DATABASE  local: the Postgres of deploy/local/compose.yaml, database ulw_dev
#                     cloud: ULW_CLOUD_DATABASE_URL (Neon, or any Postgres 16)
#   ULW_DEV_STORAGE   local: the MinIO of deploy/local/compose.yaml, bucket ulw-dev
#                     cloud: Cloudflare R2, from ULW_CLOUD_R2_ACCOUNT_ID, ULW_CLOUD_R2_BUCKET,
#                     ULW_CLOUD_R2_ACCESS_KEY_ID and ULW_CLOUD_R2_SECRET_ACCESS_KEY
#   ULW_DEV_CALLS     off (default), local (LiveKit of compose.yaml's calls profile) or cloud
#                     (LiveKit Cloud, from ULW_CLOUD_LIVEKIT_URL, ULW_CLOUD_LIVEKIT_API_KEY and
#                     ULW_CLOUD_LIVEKIT_API_SECRET)
#   ULW_DEV_BUILD     the build tree the binaries come from, default build/dev
#
# In a codespace the ULW_CLOUD_* values are Codespaces secrets; anywhere else they may be put
# in a .env file at the repository's root, which git ignores and this reads.

dev_root=$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)
dev_state=$dev_root/.dev
mkdir -p "$dev_state/logs" "$dev_state/run"

if [[ -f $dev_root/.env ]]; then
    set -a
    # shellcheck disable=SC1091
    . "$dev_root/.env"
    set +a
fi

dev_build=$(cd "$dev_root" && realpath -m "${ULW_DEV_BUILD:-build/dev}")
dev_backend=${ULW_DEV_BACKEND:-local}
dev_database=${ULW_DEV_DATABASE:-$dev_backend}
dev_storage=${ULW_DEV_STORAGE:-$dev_backend}
dev_calls=${ULW_DEV_CALLS:-off}

# The ports up.sh binds, and the ones devcontainer.json forwards.
dev_gateway_port=8080
dev_chat_port=8081
dev_chat_node_port=8082
dev_issuer=ulw-dev
dev_key=$dev_state/dev-key.json
dev_jwks=$dev_state/jwks.json

# The local services' fixed, loopback-only credentials (deploy/local/compose.yaml).
dev_local_pg=postgresql://postgres:testtest123@127.0.0.1:55432
dev_local_minio=http://127.0.0.1:9000
dev_local_minio_user=ulw-dev
dev_local_minio_secret=ulw-dev-secret
dev_local_bucket=ulw-dev

dev_fail() {
    echo "${0##*/}: $*" >&2
    exit 1
}

# Fails naming every variable of the list that is unset or empty.
dev_require() {
    local what=$1 missing=() name
    shift
    for name in "$@"; do
        [[ -n ${!name:-} ]] || missing+=("$name")
    done
    if ((${#missing[@]})); then
        dev_fail "$what needs ${missing[*]} (a Codespaces secret, or a line of .env; docs/development/cloud-dev.md)"
    fi
}

# The page origin a browser on the developer's machine uses: the Codespaces forwarder's URL
# for the gateway's port, or loopback anywhere else.
dev_origin() {
    local port=$1
    if [[ -n ${CODESPACE_NAME:-} && -n ${GITHUB_CODESPACES_PORT_FORWARDING_DOMAIN:-} ]]; then
        echo "https://$CODESPACE_NAME-$port.$GITHUB_CODESPACES_PORT_FORWARDING_DOMAIN"
    else
        echo "http://127.0.0.1:$port"
    fi
}

dev_bin() {
    local path=$dev_build/$1
    [[ -x $path ]] || dev_fail "$path is not built; run tools/dev/build.sh"
    echo "$path"
}

# The database URL of this run, without creating anything.
dev_database_url() {
    case $dev_database in
    local) echo "$dev_local_pg/ulw_dev" ;;
    cloud)
        dev_require "ULW_DEV_DATABASE=cloud" ULW_CLOUD_DATABASE_URL
        echo "$ULW_CLOUD_DATABASE_URL"
        ;;
    *) dev_fail "ULW_DEV_DATABASE: expected local or cloud, not $dev_database" ;;
    esac
}

# The storage settings of this run, one NAME=value per line, as the gateway, worker and reaper
# take them (ULW_STORAGE and the rest).
dev_storage_env() {
    case $dev_storage in
    local)
        printf '%s\n' ULW_STORAGE=minio "ULW_S3_ENDPOINT=$dev_local_minio" \
            "ULW_BUCKET=$dev_local_bucket" "ULW_S3_ACCESS_KEY_ID=$dev_local_minio_user" \
            "ULW_S3_SECRET_ACCESS_KEY=$dev_local_minio_secret"
        ;;
    cloud)
        dev_require "ULW_DEV_STORAGE=cloud" ULW_CLOUD_R2_ACCOUNT_ID ULW_CLOUD_R2_BUCKET \
            ULW_CLOUD_R2_ACCESS_KEY_ID ULW_CLOUD_R2_SECRET_ACCESS_KEY
        printf '%s\n' ULW_STORAGE=r2 "ULW_R2_ACCOUNT_ID=$ULW_CLOUD_R2_ACCOUNT_ID" \
            "ULW_BUCKET=$ULW_CLOUD_R2_BUCKET" "ULW_S3_ACCESS_KEY_ID=$ULW_CLOUD_R2_ACCESS_KEY_ID" \
            "ULW_S3_SECRET_ACCESS_KEY=$ULW_CLOUD_R2_SECRET_ACCESS_KEY"
        ;;
    *) dev_fail "ULW_DEV_STORAGE: expected local or cloud, not $dev_storage" ;;
    esac
}

# The LiveKit settings chat_server takes for calls, one NAME=value per line; none when off.
dev_calls_env() {
    case $dev_calls in
    off) ;;
    local)
        # The fake key of deploy/local/compose.yaml's LiveKit, which listens on loopback only:
        # calls placed from a browser inside this machine (the tests/call suite), not from the
        # developer's own machine through the forwarder, which carries no WebRTC media.
        printf '%s\n' LIVEKIT_API_URL=http://127.0.0.1:7880 LIVEKIT_CLIENT_URL=ws://127.0.0.1:7880 \
            LIVEKIT_API_KEY=ulw-dev-key LIVEKIT_API_SECRET=ulw-dev-secret-testtest123-not-a-real-secret
        ;;
    cloud)
        dev_require "ULW_DEV_CALLS=cloud" ULW_CLOUD_LIVEKIT_URL ULW_CLOUD_LIVEKIT_API_KEY \
            ULW_CLOUD_LIVEKIT_API_SECRET
        # LiveKit Cloud serves its API over https on the host its clients dial over wss.
        printf '%s\n' "LIVEKIT_API_URL=https://${ULW_CLOUD_LIVEKIT_URL#wss://}" \
            "LIVEKIT_CLIENT_URL=$ULW_CLOUD_LIVEKIT_URL" \
            "LIVEKIT_API_KEY=$ULW_CLOUD_LIVEKIT_API_KEY" \
            "LIVEKIT_API_SECRET=$ULW_CLOUD_LIVEKIT_API_SECRET"
        ;;
    *) dev_fail "ULW_DEV_CALLS: expected off, local or cloud, not $dev_calls" ;;
    esac
}

# The signing key the services trust and tools/dev/token.sh signs with; made once and kept in
# .dev, so tokens minted earlier stay good across restarts of the stack.
dev_ensure_key() {
    local devtoken
    devtoken=$(dev_bin tools/devtoken/ulw_devtoken)
    if [[ ! -f $dev_key ]]; then
        "$devtoken" keygen "$dev_key" >/dev/null
    fi
    "$devtoken" jwks "$dev_key" >"$dev_jwks"
}
