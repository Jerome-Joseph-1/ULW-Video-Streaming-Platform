#!/usr/bin/env bash
# Starts the platform from the build tree, in the background, against the backing services
# tools/dev/lib.sh chooses (local by default; ULW_DEV_BACKEND=cloud for Neon and R2):
#
#   ulw_migrate         the schema, applied to the database first
#   gateway_server      uploads, catalog, playback          port 8080
#   transcode_worker    the HLS transcode jobs
#   chat_server         chat, presence, calls               port 8081 (node channel 8082)
#
# Logs go to .dev/logs/<name>.log; tools/dev/down.sh stops them. Each service gets only the
# settings named here (env -i), never the codespace's secrets wholesale.
set -euo pipefail
# shellcheck source=tools/dev/lib.sh
. "$(dirname "$0")/lib.sh"

migrate=$(dev_bin apps/migrate/ulw_migrate)
gateway=$(dev_bin apps/gateway/gateway_server)
worker=$(dev_bin apps/worker/transcode_worker)
sandbox=$(dev_bin apps/worker/ulw_sandbox)
chat=$(dev_bin apps/chat/chat_server)

for name in gateway worker chat; do
    if [[ -f $dev_state/run/$name.pid ]] && kill -0 "$(cat "$dev_state/run/$name.pid")" 2>/dev/null; then
        dev_fail "$name is already running; tools/dev/down.sh first"
    fi
done

# Each in a command substitution of its own, so that a setting it refuses stops this script.
database_url=$(dev_database_url)
storage_text=$(dev_storage_env)
calls_text=$(dev_calls_env)
mapfile -t storage <<<"$storage_text"
calls=()
[[ -z $calls_text ]] || mapfile -t calls <<<"$calls_text"

if [[ $dev_database == local || $dev_storage == local ]]; then
    "$dev_root/tools/dev/services.sh" up
fi
if [[ $dev_calls == local ]]; then
    "$dev_root/tools/dev/services.sh" calls
fi
if [[ $dev_database == local ]]; then
    exists=$(psql -X -At -d "$dev_local_pg/postgres" -c "SELECT 1 FROM pg_database WHERE datname = 'ulw_dev'")
    [[ $exists == 1 ]] || psql -X -q -d "$dev_local_pg/postgres" -c "CREATE DATABASE ulw_dev"
fi
if [[ $dev_storage == local ]]; then
    # 200 made it, 409 it was there already (BucketAlreadyOwnedByYou).
    status=$(curl -sS -o /dev/null -w '%{http_code}' -X PUT --aws-sigv4 aws:amz:us-east-1:s3 \
        --user "$dev_local_minio_user:$dev_local_minio_secret" "$dev_local_minio/$dev_local_bucket")
    [[ $status == 200 || $status == 409 ]] || dev_fail "MinIO answered $status creating bucket $dev_local_bucket"
fi

dev_ensure_key
scratch=$dev_state/scratch
mkdir -p "$scratch"
chmod 0700 "$scratch"

# What every service takes. Root is refused unless allowed; a container may run as root.
common=("PATH=$PATH" "ULW_DATABASE_URL=$database_url" ULW_LOG_LEVEL="${ULW_LOG_LEVEL:-info}")
if [[ $(id -u) == 0 ]]; then
    common+=(ULW_ALLOW_ROOT=1)
fi
[[ -z ${ULW_REACTOR:-} ]] || common+=("ULW_REACTOR=$ULW_REACTOR")
# The dev key set: development only (ULW_DEV_MODE), and the services refuse it in a pod.
auth=(ULW_DEV_MODE=1 "ULW_DEV_JWKS_FILE=$dev_jwks" "JWT_ISSUER=$dev_issuer")
# A browser's page may come from the forwarder's URL of either port, or from loopback.
origins="$(dev_origin "$dev_gateway_port"),$(dev_origin "$dev_chat_port")"
[[ -z ${ULW_DEV_EXTRA_ORIGINS:-} ]] || origins+=",$ULW_DEV_EXTRA_ORIGINS"

echo "up: migrating ($dev_database database)"
env -i "${common[@]}" "$migrate" >"$dev_state/logs/migrate.log" 2>&1 ||
    dev_fail "ulw_migrate failed; see .dev/logs/migrate.log"

start() {
    local name=$1 file=$2
    shift 2
    : >"$dev_state/logs/$name.log"
    env -i "$@" setsid "$file" >>"$dev_state/logs/$name.log" 2>&1 </dev/null &
    echo $! >"$dev_state/run/$name.pid"
}

start gateway "$gateway" "${common[@]}" "${storage[@]}" "${auth[@]}" \
    "ULW_LISTEN_PORT=$dev_gateway_port" "ULW_ALLOWED_ORIGINS=$origins"
start worker "$worker" "${common[@]}" "${storage[@]}" \
    ULW_NODE_ID=dev-worker "ULW_SCRATCH_DIR=$scratch" "ULW_SANDBOX_BIN=$sandbox"
# One chat node. Its node channel secret is made per start: there are no other nodes to share
# it with.
start chat "$chat" "${common[@]}" "${auth[@]}" "${calls[@]}" \
    ULW_NODE_ID=dev-chat-1 "ULW_LISTEN_PORT=$dev_chat_port" \
    "ULW_NODE_ADDRESS=127.0.0.1:$dev_chat_node_port" ULW_DEV_LOOPBACK_NODES=1 \
    "ULW_NODE_SECRET=$(head -c 32 /dev/urandom | od -An -tx1 | tr -d ' \n')" \
    "ULW_ALLOWED_ORIGINS=$origins" ULW_CHAT_SELF_SERVICE=on

# Ready when each has said which port it took, or has gone.
for name in gateway chat; do
    port=$([[ $name == gateway ]] && echo "$dev_gateway_port" || echo "$dev_chat_port")
    for _ in $(seq 1 100); do
        grep -q "\"port\":$port" "$dev_state/logs/$name.log" && break
        kill -0 "$(cat "$dev_state/run/$name.pid")" 2>/dev/null ||
            dev_fail "$name exited; see .dev/logs/$name.log"
        sleep 0.1
    done
    grep -q "\"port\":$port" "$dev_state/logs/$name.log" ||
        dev_fail "$name did not report its port in 10 s; see .dev/logs/$name.log"
done
sleep 1
kill -0 "$(cat "$dev_state/run/worker.pid")" 2>/dev/null ||
    dev_fail "transcode_worker exited; see .dev/logs/worker.log"

cat <<EOF
up: running (database: $dev_database, storage: $dev_storage, calls: $dev_calls)
  gateway   http://127.0.0.1:$dev_gateway_port   from your own machine: $(dev_origin "$dev_gateway_port")
  chat      ws://127.0.0.1:$dev_chat_port/rt     from your own machine: $(dev_origin "$dev_chat_port" | sed 's/^http/ws/')/rt
  logs      .dev/logs/   (tools/dev/logs.sh [gateway|worker|chat])
  token     tools/dev/token.sh <user>
  stop      tools/dev/down.sh
EOF
