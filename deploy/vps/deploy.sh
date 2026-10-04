#!/usr/bin/env bash
# Deploys deploy/vps/compose.yaml from this checkout to /opt/ulw on this host, as compose project
# `ulw-prod`, and waits until every service is healthy. Run by .github/workflows/deploy-vps.yml
# on the host's self-hosted runner; a person with sudo on the host can run it the same way from
# a checkout (README.md, "Redeploy").
#
#   deploy/vps/deploy.sh <public ip> <gateway image> <worker image> <chat image>
#
# /opt/ulw is outside every runner's workspace, so no job's cleanup reaches it. Secrets are made
# here once, into /opt/ulw/.env (root, 0600), and never printed; later deploys keep them. Only the
# project's own containers are touched: nothing here prunes, stops or removes anything else.
set -euo pipefail

ip=${1:?public ip}
gateway_image=${2:?gateway image}
worker_image=${3:?worker image}
chat_image=${4:?chat image}
[[ $ip =~ ^[0-9]{1,3}(\.[0-9]{1,3}){3}$ ]] || { echo "deploy: '$ip' is not an IPv4 address" >&2; exit 1; }
host="${ip//./-}.sslip.io"

here=$(cd "$(dirname "$0")" && pwd)
root=$(cd "$here/../.." && pwd)
dir=/opt/ulw

echo "deploy: https://$host into $dir"
sudo install -d -m 0755 -o root -g root "$dir" "$dir/keycloak"

# The files compose.yaml names, from this checkout.
sudo install -m 0644 "$here/compose.yaml" "$dir/compose.yaml"
sudo install -m 0644 "$here/Caddyfile" "$dir/Caddyfile"
sudo install -m 0644 "$here/setup-bucket.mjs" "$dir/setup-bucket.mjs"
sudo install -m 0644 "$root/deploy/kubernetes/cluster/seccomp/ulw-worker.json" "$dir/seccomp.json"
sed "s|__ULW_HOST__|$host|g" "$here/keycloak/ulw-realm.json.template" |
    sudo install -m 0644 /dev/stdin "$dir/keycloak/ulw-realm.json"

# The web app and the OpenMLS client, replaced whole; config.json says to sign in on Keycloak.
stage=$(mktemp -d)
trap 'rm -rf "$stage"' EXIT
cp -a "$root/demo/web" "$stage/web"
cp -a "$root/clients/web-mls/dist" "$stage/web-mls"
printf '{"auth":"oidc","label":"","issuer":"https://%s/auth/realms/ulw","clientId":"ulw-web","userClaim":"preferred_username"}\n' \
    "$host" >"$stage/web/config.json"
chmod -R a+rX "$stage"
for d in web web-mls; do
    sudo rm -rf "$dir/$d.new"
    sudo cp -a "$stage/$d" "$dir/$d.new"
    sudo chown -R root:root "$dir/$d.new"
    sudo rm -rf "$dir/$d.old"
    if sudo test -e "$dir/$d"; then sudo mv "$dir/$d" "$dir/$d.old"; fi
    sudo mv "$dir/$d.new" "$dir/$d"
    sudo rm -rf "$dir/$d.old"
done

# Secrets: once, on this host only. Hex, so they need no quoting in URLs or YAML.
if ! sudo test -s "$dir/.env"; then
    echo "deploy: generating $dir/.env (first deploy)"
    sudo bash -c 'umask 077
        r() { openssl rand -hex "$1"; }
        {
            echo "# Generated on this host by deploy/vps/deploy.sh at the first deploy. Never commit or share."
            echo "ULW_DB_PASSWORD=$(r 24)"
            echo "KC_DB_PASSWORD=$(r 24)"
            echo "KC_ADMIN_PASSWORD=$(r 18)"
            echo "MINIO_ROOT_USER=ulw$(r 6)"
            echo "MINIO_ROOT_PASSWORD=$(r 24)"
            echo "LIVEKIT_API_KEY=ulw$(r 6)"
            echo "LIVEKIT_API_SECRET=$(r 32)"
            echo "ULW_NODE_SECRET=$(r 32)"
        } >"$1/.env.tmp"
        mv "$1/.env.tmp" "$1/.env"' bash "$dir"
fi
sudo chown root:root "$dir/.env"
sudo chmod 0600 "$dir/.env"

# What is not secret and may change with each deploy.
printf 'ULW_HOST=%s\nULW_PUBLIC_IP=%s\nULW_IMAGE_GATEWAY=%s\nULW_IMAGE_WORKER=%s\nULW_IMAGE_CHAT=%s\n' \
    "$host" "$ip" "$gateway_image" "$worker_image" "$chat_image" |
    sudo install -m 0644 /dev/stdin "$dir/deploy.env"

# The one way to run compose for this project (README.md): sudo /opt/ulw/compose.sh <args>.
sudo install -m 0755 /dev/stdin "$dir/compose.sh" <<'EOF'
#!/usr/bin/env bash
# docker compose for the ulw-prod project in /opt/ulw, with its secrets and settings.
exec docker compose --project-directory /opt/ulw --project-name ulw-prod \
    --env-file /opt/ulw/.env --env-file /opt/ulw/deploy.env -f /opt/ulw/compose.yaml "$@"
EOF
compose() { sudo "$dir/compose.sh" "$@"; }

# Fails here, before anything changes, if a variable is missing. Prints nothing (secrets).
compose config --quiet

# The firewall: the public ports compose.yaml publishes, when ufw is on. Nothing else changes.
if sudo ufw status 2>/dev/null | grep -q '^Status: active'; then
    for rule in 80/tcp 443/tcp 443/udp 7801/tcp 7802/udp 3478/udp 5349/tcp; do
        sudo ufw allow "$rule" comment 'ulw-prod' >/dev/null
        echo "deploy: ufw allows $rule"
    done
else
    echo "deploy: ufw is not active; no firewall change"
fi

compose pull --quiet
compose up -d --remove-orphans

# Every long-running service healthy (or running, where it has no healthcheck), and the one-shot
# setup containers exited 0.
deadline=$((SECONDS + 900))
while :; do
    bad=$(compose ps -a --format '{{.Service}}|{{.State}}|{{.Health}}|{{.ExitCode}}' | awk -F'|' '
        $1 == "bucket" || $1 == "migrate" { if ($2 != "exited" || $4 != 0) print; next }
        $2 != "running" || ($3 != "" && $3 != "healthy") { print }')
    [[ -z $bad ]] && break
    if ((SECONDS > deadline)); then
        echo "deploy: not healthy after 15 minutes:" >&2
        echo "$bad" >&2
        exit 1
    fi
    if grep -qE '^(bucket|migrate)\|exited' <<<"$bad"; then
        echo "deploy: a setup step failed:" >&2
        echo "$bad" >&2
        exit 1
    fi
    sleep 5
done
compose ps --format 'table {{.Service}}\t{{.State}}\t{{.Health}}'

# From outside the namespace, through the published port and the real certificate.
for path in /api/v1/readyz /auth/realms/ulw/.well-known/openid-configuration /config.json /; do
    code=$(curl -sS -o /dev/null -w '%{http_code}' --max-time 20 "https://$host$path")
    echo "deploy: GET https://$host$path -> $code"
    [[ $code == 200 ]] || exit 1
done
echo "deploy: https://$host is up"
