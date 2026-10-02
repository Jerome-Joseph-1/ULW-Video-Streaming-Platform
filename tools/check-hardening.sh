#!/usr/bin/env bash
# Usage: tools/check-hardening.sh PATH...
#
# Checks that the servers and tools this repository ships were built hardened, with readelf
# alone (binutils), so nothing is downloaded (docs/adr/0072). Each PATH is either an ELF
# executable or a build tree (build/ci, build/release), from which the executables under apps/
# and tools/ are taken; a build tree's compile_commands.json is also checked.
#
# Per executable:
#   PIE            ELF type DYN with DF_1_PIE
#   full RELRO     a GNU_RELRO segment and BIND_NOW
#   NX             GNU_STACK not executable
#   no RWX         no loadable segment both writable and executable
#   stack guard    imports __stack_chk_fail (-fstack-protector-strong)
#   CET            .note.gnu.property marks IBT and SHSTK (-fcf-protection, on every object)
# FORTIFY_SOURCE leaves a trace only where a call's bounds were unknown at compile time, so a
# binary without a __*_chk import proves nothing either way. Instead: at least one executable
# must import one, and every first-party translation unit in a build tree's
# compile_commands.json, and every vendored one linked into the servers (llhttp, srt), must be
# compiled with -D_FORTIFY_SOURCE=3, -fstack-protector-strong, -fstack-clash-protection and
# -fcf-protection.
#
# Sanitizer builds are refused: they are not what ships, and ASan changes the layout this checks.
set -euo pipefail

if [[ $# -eq 0 ]]; then
    echo "usage: $0 BUILD_DIR_OR_EXECUTABLE..." >&2
    exit 2
fi

failures=0
fortified=0
executables=()
compile_dbs=()

fail() {
    echo "FAIL $1: $2"
    failures=$((failures + 1))
}

is_elf() {
    [[ -f $1 && $(head -c 4 "$1" | od -An -c | tr -d ' ') == '177ELF' ]]
}

for path in "$@"; do
    if [[ -d $path ]]; then
        if [[ -f $path/CMakeCache.txt ]] && grep -Eq '^ULW_SANITIZE:STRING=.+' "$path/CMakeCache.txt"; then
            echo "$path: a sanitizer build (ULW_SANITIZE); check the ci or release build" >&2
            exit 2
        fi
        found=0
        while IFS= read -r -d '' file; do
            if is_elf "$file"; then
                executables+=("$file")
                found=1
            fi
        done < <(find "$path/apps" "$path/tools" -type f -perm -u+x -not -path '*/CMakeFiles/*' \
            -print0 2>/dev/null | sort -z)
        if [[ $found -eq 0 ]]; then
            echo "$path: no executables under apps/ or tools/; build it first" >&2
            exit 2
        fi
        [[ -f $path/compile_commands.json ]] && compile_dbs+=("$path")
    elif is_elf "$path"; then
        executables+=("$path")
    else
        echo "$path: neither a build tree nor an ELF file" >&2
        exit 2
    fi
done

for exe in "${executables[@]}"; do
    name=${exe#./}
    header=$(readelf -hW "$exe")
    segments=$(readelf -lW "$exe")
    dynamic=$(readelf -dW "$exe")
    dynsyms=$(readelf --dyn-syms -W "$exe")
    notes=$(readelf -nW "$exe")

    # Into a variable first: with pipefail, grep -q leaving early would fail readelf's write.
    symbols=$(readelf -sW "$exe")
    if grep -Eq ' (__asan_init|__tsan_init|__ubsan_handle_[a-z_]+|__msan_init)$' <<<"$symbols"; then
        echo "$name: a sanitizer build; check the ci or release build" >&2
        exit 2
    fi

    ok=1
    if ! grep -Eq 'Type:[[:space:]]+DYN' <<<"$header" || ! grep -Eq 'FLAGS_1.*\bPIE\b' <<<"$dynamic"; then
        fail "$name" "not PIE"; ok=0
    fi
    if ! grep -q 'GNU_RELRO' <<<"$segments"; then
        fail "$name" "no RELRO segment"; ok=0
    fi
    if ! grep -Eq '\(FLAGS\).*\bBIND_NOW\b|\(FLAGS_1\).*\bNOW\b' <<<"$dynamic"; then
        fail "$name" "partial RELRO: not linked with -z now"; ok=0
    fi
    stack=$(awk '$1 == "GNU_STACK" { print $7 }' <<<"$segments")
    if [[ -z $stack || $stack == *E* ]]; then
        fail "$name" "executable or unmarked stack (GNU_STACK '${stack:-missing}')"; ok=0
    fi
    # Program headers: Type Offset VirtAddr PhysAddr FileSiz MemSiz Flg Align; Flg is one to
    # three of R, W and E, written with spaces between them ("R E", "RW ").
    if grep -E '^[[:space:]]+LOAD' <<<"$segments" | grep -q ' RWE '; then
        fail "$name" "a loadable segment is writable and executable"; ok=0
    fi
    if ! grep -q ' __stack_chk_fail' <<<"$dynsyms"; then
        fail "$name" "no stack protector (__stack_chk_fail not imported)"; ok=0
    fi
    features=$(grep -o 'x86 feature: .*' <<<"$notes" || true)
    if [[ $features != *IBT* || $features != *SHSTK* ]]; then
        fail "$name" "no IBT/SHSTK marking (an object without -fcf-protection): '${features:-none}'"
        ok=0
    fi
    chk=$(grep -Ec ' __[a-z0-9_]+_chk(@|$)' <<<"$dynsyms" || true)
    fortified=$((fortified + chk))
    [[ $ok -eq 1 ]] && echo "ok   $name (PIE, full RELRO, NX, no RWX, stack guard, IBT+SHSTK, $chk fortified imports)"
done

if [[ $fortified -eq 0 ]]; then
    fail "all" "no executable imports a __*_chk function: FORTIFY_SOURCE is not in effect"
fi

for dir in "${compile_dbs[@]}"; do
    root=$(cd "$(dirname "$0")/.." && pwd)
    missing=$(python3 - "$dir/compile_commands.json" "$root" <<'EOF'
import json
import shlex
import sys

db, root = sys.argv[1], sys.argv[2]
required = ("-D_FORTIFY_SOURCE=3", "-fstack-protector-strong", "-fstack-clash-protection",
            "-fcf-protection")
for entry in json.load(open(db, encoding="utf-8")):
    source = entry["file"]
    vendored = "/_deps/llhttp-src/" in source or "/_deps/srt-src/" in source
    if not vendored and ("/_deps/" in source or not source.startswith(root + "/")):
        continue
    args = entry.get("arguments") or shlex.split(entry["command"])
    lacking = [flag for flag in required if flag not in args]
    if lacking:
        print(f"{source.removeprefix(root + '/')}: {' '.join(lacking)}")
EOF
    )
    if [[ -n $missing ]]; then
        while IFS= read -r line; do fail "$dir/compile_commands.json" "$line"; done <<<"$missing"
    else
        echo "ok   $dir/compile_commands.json (every first-party, llhttp and srt unit hardened)"
    fi
done

if [[ $failures -gt 0 ]]; then
    echo "$failures hardening check(s) failed"
    exit 1
fi
