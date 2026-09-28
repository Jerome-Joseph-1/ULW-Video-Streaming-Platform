#!/usr/bin/env bash
# M2 acceptance run for one reactor. Prints one line per measurement and exits non-zero on the
# first check that fails.
# Usage: tests/load/echo_acceptance.sh <build-dir> <io_uring|epoll> [seconds]
set -euo pipefail
build=$1
reactor=$2
secs=${3:-60}
port=$((7200 + RANDOM % 500))
server=$build/tests/ulw_echo_server
loadgen=$build/tests/ulw_loadgen

pingpong_clients=500
noread_clients=50
idle_clients=5000
# Bounds, from local runs of this script on both reactors (kernel 6.18, 4 vCPU):
#  - Pingpong made about 160,000 round trips/s. A rate 160 times lower is a stalled server,
#    not a slow runner.
min_rt_per_sec=1000
#  - Never-reading clients left RSS flat between the samples. Backpressure caps each queue at
#    the 256 KiB watermark plus one 64 KiB read, so RSS may move by one such queue at most; a
#    server that kept reading would grow at the clients' write rate, hundreds of MB/s.
max_noread_growth_kb=$((256 + 64))
#  - An idle connection cost 60 to 370 bytes. 1 KiB each still fails a design with a
#    per-connection read buffer, which costs 64 KiB each.
max_idle_growth_kb=$idle_clients

work=$(mktemp -d)
pid=
lg=
trap 'kill -KILL $pid $lg 2>/dev/null || true; rm -rf "$work"' EXIT

fail() {
    echo "FAILED: $*" >&2
    exit 1
}
rss() { awk '/VmRSS/ {print $2}' "/proc/$1/status"; }
fds() { find "/proc/$1/fd" -mindepth 1 -maxdepth 1 | wc -l; }
fds_are() { [[ $(fds "$1") == "$2" ]]; }
# The value of key=value in a loadgen report.
field() { tr ' ' '\n' <"$1" | sed -n "s/^$2=//p"; }
# An exited child stays a zombie until reaped, and kill -0 cannot tell that from a live one.
exited() { [[ ! -e /proc/$1 ]] || grep -q '^State:[[:space:]]*Z' "/proc/$1/status" 2>/dev/null; }

# Polls a command until it succeeds or $1 seconds pass.
wait_for() {
    local deadline=$((SECONDS + $1))
    shift
    until "$@"; do
        ((SECONDS < deadline)) || return 1
        sleep 0.1
    done
}

# Waits at most $2 seconds for child $1 and returns its exit status.
reap() {
    wait_for "$2" exited "$1" || fail "pid $1 still running after $2 s"
    local code=0
    wait "$1" || code=$?
    return "$code"
}

start_server() {
    # Emptied here, not by the child's redirect, which can run after the wait below has already
    # found the previous server's "listening" and sampled a process still starting up.
    : >"$work/server.log"
    "$server" --port "$port" --reactor "$reactor" --idle-ms 600000 >>"$work/server.log" 2>&1 &
    pid=$!
    wait_for 5 grep -q listening "$work/server.log" || fail "server did not start: $(cat "$work/server.log")"
    # A silent fallback would pass the io_uring run on epoll.
    grep -q "reactor=$reactor " "$work/server.log" || fail "wrong reactor: $(head -1 "$work/server.log")"
}

# With no client left the drain has nothing to wait for, but the bound is the server's own
# promise: its 30 s drain deadline plus one 1 s loop wait.
stop_server() {
    kill -TERM "$pid"
    local code=0
    reap "$pid" 31 || code=$?
    echo "sigterm exit=$code $(tail -1 "$work/server.log")"
    ((code == 0)) || fail "server exited with $code on SIGTERM"
    grep -q '^drained' "$work/server.log" || fail "server did not drain"
    pid=
}

# The loadgen's exit status covers transport errors and mismatched echo bytes. A server that
# drops clients or barely echoes shows only in the counters, which the caller checks.
run_loadgen() {
    local out=$1
    shift
    "$loadgen" --port "$port" "$@" >"$out" || fail "loadgen $*: $(cat "$out")"
}

# Closing a vanished client's socket takes one loop iteration; 5 s absorbs a loaded runner.
expect_fds_back() {
    wait_for 5 fds_are "$pid" "$1" || fail "$2: fds=$(fds "$pid"), baseline $1"
}

start_server
base_fds=$(fds "$pid")
echo "reactor=$reactor $(head -1 "$work/server.log")"
echo "baseline rss_kb=$(rss "$pid") fds=$base_fds"

reply=$(printf 'hello nc\n' | timeout 2 nc -q1 127.0.0.1 "$port") || true
[[ $reply == "hello nc" ]] || fail "nc echo: '$reply'"
echo "nc echo ok"

run_loadgen "$work/pingpong.out" --connections "$pingpong_clients" --seconds "$secs" --mode pingpong
echo "pingpong ${pingpong_clients}x${secs}s: $(cat "$work/pingpong.out")"
(($(field "$work/pingpong.out" open_at_end) == pingpong_clients)) || fail "pingpong clients were dropped"
(($(field "$work/pingpong.out" rt_per_sec) >= min_rt_per_sec)) || fail "pingpong below $min_rt_per_sec rt/s"
expect_fds_back "$base_fds" "after pingpong"
echo "after pingpong rss_kb=$(rss "$pid") fds=$(fds "$pid")"

"$loadgen" --port "$port" --connections "$noread_clients" --seconds 20 --mode noread >"$work/noread.out" &
lg=$!
sleep 5
r1=$(rss "$pid")
sleep 12
r2=$(rss "$pid")
reap "$lg" 10 || fail "noread loadgen: $(cat "$work/noread.out")"
lg=
echo "noread ${noread_clients} clients: rss_kb at 5s=$r1 at 17s=$r2 ($(cat "$work/noread.out"))"
(($(field "$work/noread.out" open_at_end) == noread_clients)) || fail "noread clients were dropped"
((r2 - r1 <= max_noread_growth_kb)) || fail "RSS grew $((r2 - r1)) kB under noread, bound $max_noread_growth_kb"
expect_fds_back "$base_fds" "after noread"
stop_server

# Idle footprint on a fresh process, so earlier traffic does not inflate it.
start_server
base_rss=$(rss "$pid")
base_fds=$(fds "$pid")
"$loadgen" --port "$port" --connections "$idle_clients" --seconds 15 --mode idle >"$work/idle.out" &
lg=$!
sleep 12
with_rss=$(rss "$pid")
with_fds=$(fds "$pid")
echo "${idle_clients} idle (fresh): baseline rss_kb=$base_rss with_${idle_clients} rss_kb=$with_rss fds=$with_fds"
reap "$lg" 10 || fail "idle loadgen: $(cat "$work/idle.out")"
lg=
(($(field "$work/idle.out" open_at_end) == idle_clients)) || fail "idle clients were dropped"
((with_fds == base_fds + idle_clients)) || fail "server held $((with_fds - base_fds)) of $idle_clients idle clients"
((with_rss - base_rss <= max_idle_growth_kb)) || fail "idle RSS grew $((with_rss - base_rss)) kB, bound $max_idle_growth_kb"
expect_fds_back "$base_fds" "after idle"
echo "after idle rss_kb=$(rss "$pid") fds=$(fds "$pid")"
stop_server
