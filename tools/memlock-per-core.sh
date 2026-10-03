#!/usr/bin/env bash
# Usage: tools/memlock-per-core.sh <command> [arg...]
# Runs the command with a max locked memory (RLIMIT_MEMLOCK) of at least 8 MiB per core, for a
# command that runs one test per core (ctest -j "$(nproc)"). Since Linux 6.14 every io_uring
# ring's pages are charged to a locked-memory counter that all of a user's processes share,
# checked against the limit of the process that creates the ring. 8 MiB is the kernel's and
# systemd's default, and a host's worth for one test; a limit that is already higher is kept
# (ADR-0090). Raising the hard limit needs sudo.
set -euo pipefail

if [ "$#" -eq 0 ]; then
    echo "usage: $0 <command> [arg...]" >&2
    exit 2
fi

want=$((8 * 1024 * 1024 * $(nproc)))
have=$(ulimit -l)
if [ "$have" != unlimited ] && [ $((have * 1024)) -lt "$want" ]; then
    # A higher hard limit stays as it is.
    hard=$(ulimit -H -l)
    if [ "$hard" != unlimited ]; then
        hard=$((hard * 1024 > want ? hard * 1024 : want))
    fi
    sudo prlimit --pid $$ --memlock="$want:$hard"
fi
echo "max locked memory: $(ulimit -l) KiB (soft), $(ulimit -H -l) KiB (hard), for $(nproc) cores" >&2
exec "$@"
