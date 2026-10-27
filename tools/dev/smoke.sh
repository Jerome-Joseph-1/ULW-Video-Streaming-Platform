#!/usr/bin/env bash
# One video through the running stack (tools/dev/up.sh), whichever backing services it uses:
# makes a 4 s test clip with ffmpeg, uploads it, commits it, waits for the worker to make it
# ready, then fetches its master playlist and one segment through the presigned URL.
#
#   tools/dev/smoke.sh [gateway URL, default http://127.0.0.1:8080]
set -euo pipefail
# shellcheck source=tools/dev/lib.sh
. "$(dirname "$0")/lib.sh"

gateway=${1:-http://127.0.0.1:$dev_gateway_port}
token=$("$dev_root/tools/dev/token.sh" smoke-user)
auth=(-H "Authorization: Bearer $token")
clip=$dev_state/smoke.mp4

if [[ ! -f $clip ]]; then
    ffmpeg -loglevel error -f lavfi -i testsrc2=size=640x360:rate=30 -f lavfi \
        -i sine=frequency=440:sample_rate=48000 -t 4 -c:v libx264 -pix_fmt yuv420p -c:a aac \
        -movflags +faststart "$clip"
fi
size=$(stat -c %s "$clip")

echo "smoke: create"
created=$(curl -fsS "${auth[@]}" -H 'Content-Type: application/json' \
    -d "{\"filename\":\"smoke.mp4\",\"size_bytes\":$size,\"content_type\":\"video/mp4\"}" \
    "$gateway/api/v1/uploads")
upload=$(jq -r .upload_id <<<"$created")
video=$(jq -r .video_id <<<"$created")

echo "smoke: append $size bytes"
offset=$(curl -fsS "${auth[@]}" -X PATCH -H 'Upload-Offset: 0' \
    -H 'Content-Type: application/offset+octet-stream' --data-binary "@$clip" \
    -D - -o /dev/null "$gateway/api/v1/uploads/$upload" |
    tr -d '\r' | awk -F': ' 'tolower($1) == "upload-offset" { print $2 }')
[[ $offset == "$size" ]] || dev_fail "durable offset $offset after the append, expected $size"

echo "smoke: commit"
curl -fsS "${auth[@]}" -X POST "$gateway/api/v1/uploads/$upload/commit" >/dev/null

echo -n "smoke: transcoding"
for _ in $(seq 1 120); do
    state=$(curl -fsS "${auth[@]}" "$gateway/api/v1/videos/$video" | jq -r .state)
    [[ $state == ready || $state == failed ]] && break
    echo -n .
    sleep 1
done
echo " $state"
[[ $state == ready ]] || dev_fail "video $video is $state; see .dev/logs/worker.log"

master=$(curl -fsS "${auth[@]}" "$gateway/api/v1/videos/$video/master.m3u8")
rendition=$(grep -v '^#' <<<"$master" | head -n 1)
media=$(curl -fsS "${auth[@]}" "$gateway$rendition")
segment=$(grep -v '^#' <<<"$media" | head -n 1)
bytes=$(curl -fsS -o /dev/null -w '%{size_download}' "$segment")
echo "smoke: ok: video $video ready; first segment $bytes bytes from ${segment%%\?*}"
