#!/usr/bin/env bash
# Prints a JWT for <user> that the services tools/dev/up.sh started accept, signed with the dev
# key in .dev (ADR-0088):
#
#   tools/dev/token.sh alice
#   tools/dev/token.sh alice --email alice@example.com    (any of ulw_devtoken mint's options:
#                                                          --aud, --email, --ttl, --scope, --client)
#   curl -H "Authorization: Bearer $(tools/dev/token.sh alice)" http://127.0.0.1:8080/api/v1/...
set -euo pipefail
# shellcheck source=tools/dev/lib.sh
. "$(dirname "$0")/lib.sh"

(($#)) || dev_fail "usage: tools/dev/token.sh <user> [ulw_devtoken mint options]"
user=$1
shift
dev_ensure_key
exec "$(dev_bin tools/devtoken/ulw_devtoken)" mint "$dev_key" --iss "$dev_issuer" --sub "$user" \
    --ttl "${ULW_DEV_TOKEN_TTL:-86400}" "$@"
