#!/usr/bin/env bash
# M2 acceptance run for one reactor. Prints one line per measurement.
# Usage: tests/load/echo_acceptance.sh <build-dir> <io_uring|epoll> [seconds]
set -euo pipefail
build=$1
reactor=$2
secs=${3:-60}
port=$((7200 + RANDOM % 500))
server=$build/tests/ulw_echo_server
loadgen=$build/tests/ulw_loadgen

rss() { awk '/VmRSS/ {print $2}' "/proc/$1/status"; }
fds() { ls "/proc/$1/fd" | wc -l; }

"$server" --port "$port" --reactor "$reactor" --idle-ms 600000 >"/tmp/echo-$reactor.log" 2>&1 &
pid=$!
trap 'kill -KILL $pid 2>/dev/null || true' EXIT
for _ in $(seq 50); do grep -q listening "/tmp/echo-$reactor.log" && break; sleep 0.1; done
echo "reactor=$reactor $(head -1 /tmp/echo-$reactor.log)"
echo "baseline rss_kb=$(rss $pid) fds=$(fds $pid)"

reply=$(printf 'hello nc\n' | timeout 2 nc -q1 127.0.0.1 "$port")
[[ $reply == "hello nc" ]] && echo "nc echo ok" || { echo "nc echo FAILED: '$reply'"; exit 1; }

echo "pingpong 500x${secs}s: $("$loadgen" --port "$port" --connections 500 --seconds "$secs" --mode pingpong)"
echo "after pingpong rss_kb=$(rss $pid) fds=$(fds $pid)"

"$loadgen" --port "$port" --connections 50 --seconds 20 --mode noread >/tmp/noread.out &
lg=$!
sleep 5
r1=$(rss $pid)
sleep 12
r2=$(rss $pid)
wait $lg || true
echo "noread 50 clients: rss_kb at 5s=$r1 at 17s=$r2 ($(cat /tmp/noread.out | tr ' ' '\n' | grep -E 'bytes_written|errors' | tr '\n' ' '))"

stop() {
    kill -TERM "$1"
    set +e
    wait "$1"
    local code=$?
    set -e
    echo "sigterm exit=$code $(tail -1 /tmp/echo-$reactor.log)"
}
stop $pid

# Idle footprint on a fresh process, so earlier traffic does not inflate it.
"$server" --port "$port" --reactor "$reactor" --idle-ms 600000 >"/tmp/echo-$reactor.log" 2>&1 &
pid=$!
for _ in $(seq 50); do grep -q listening "/tmp/echo-$reactor.log" && break; sleep 0.1; done
base=$(rss $pid)
"$loadgen" --port "$port" --connections 5000 --seconds 15 --mode idle >/tmp/idle.out &
lg=$!
sleep 12
echo "5000 idle (fresh): baseline rss_kb=$base with_5000 rss_kb=$(rss $pid) fds=$(fds $pid)"
wait $lg || true
sleep 1
echo "after idle rss_kb=$(rss $pid) fds=$(fds $pid)"
stop $pid
trap - EXIT
