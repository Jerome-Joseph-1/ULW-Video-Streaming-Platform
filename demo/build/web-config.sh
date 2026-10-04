#!/bin/sh
# Writes the page's config.js at container start (nginx runs /docker-entrypoint.d/*.sh first),
# so one web image serves any deployment. Every variable is optional; unset or empty means the
# demo's own: the gateway, chat and the token issuer on the page's origin.
#   ULW_WEB_API_BASE        the gateway's base URL, e.g. https://api.example (no trailing /api)
#   ULW_WEB_CHAT_URL        chat's WebSocket URL, e.g. wss://chat.example/rt
#   ULW_WEB_TOKEN_URL       where the page gets a token (the demo's POST /auth/token?sub=<user>)
#   ULW_WEB_AUTH            dev (the demo's token issuer) or oidc
#   ULW_WEB_OIDC_ISSUER     with oidc: the issuer, e.g. https://<ip>.sslip.io/realms/ulw
#   ULW_WEB_OIDC_CLIENT_ID  with oidc: the page's public client id
set -eu
out=${ULW_WEB_CONFIG_FILE:-/usr/share/nginx/html/config.js}
json() { printf '%s' "$1" | sed -e 's/\\/\\\\/g' -e 's/"/\\"/g'; }
{
    echo '// Written at container start from ULW_WEB_* (web-config.sh).'
    printf 'window.ULW_CONFIG = {'
    printf '"apiBase":"%s",' "$(json "${ULW_WEB_API_BASE:-}")"
    printf '"chatUrl":"%s",' "$(json "${ULW_WEB_CHAT_URL:-}")"
    printf '"tokenUrl":"%s",' "$(json "${ULW_WEB_TOKEN_URL:-}")"
    printf '"auth":"%s",' "$(json "${ULW_WEB_AUTH:-dev}")"
    printf '"oidcIssuer":"%s",' "$(json "${ULW_WEB_OIDC_ISSUER:-}")"
    printf '"oidcClientId":"%s"' "$(json "${ULW_WEB_OIDC_CLIENT_ID:-}")"
    echo '};'
} > "$out.tmp"
mv "$out.tmp" "$out"
echo "web-config.sh: wrote $out"
