#!/usr/bin/env python3
"""A small source-mutation driver: it changes one thing in a source file, rebuilds one test
target, and records whether the test binary notices.

Usage:
  tools/mutate.py --build-dir build/mut --target http_unit_tests \
      --files http/src/request_parser.cpp --sample 60 --out results.jsonl \
      -- build/mut/tests/http_unit_tests --gtest_brief=1
  tools/mutate.py --list --files http/src/request_parser.cpp      # print the mutants only
  tools/mutate.py ... --lines apps/gateway/src/connection.cpp:97-461  # part of a file
  tools/mutate.py ... --only 'http/src/request_parser.cpp:120:ror:3'  # re-run chosen mutants

Mutations, applied one at a time on formatted source (binary operators are spaced):
  ror    a relational operator (< <= > >= == !=) replaced by each of the others
  lcr    && replaced by || and the reverse
  neg    the condition of an if or while negated
  del    an expression statement deleted
  ret    return true/false swapped, or a void function's early `return;` deleted
  const  an integer literal changed by +1 and by -1
  minmax std::min and std::max swapped (also std::ranges::)

A mutant that fails to compile is recorded as `unbuildable` and left out of the score. One that
builds is `killed` if the tests fail, `timeout` if they outlast --timeout, and `survived` if they
pass. The file is restored, byte for byte, after every mutant and when the run is stopped by
SIGINT, SIGTERM or SIGHUP, and the target is rebuilt at the end (a further stop signal cuts
that rebuild short); SIGKILL leaves the mutant in place (`git checkout` the file). A build or
test that outlasts its timeout is killed with its whole process group. Each mutant's id is file:line:operator:index, stable for a given source.

Mutants build with CCACHE_READONLY so they do not fill the cache; the restored source hits it.
"""
import argparse
import json
import os
import random
import re
import signal
import subprocess
import sys
import time
from pathlib import Path

# tools/pathguard.py, which keeps each path given on the command line inside the repository
# and the temporary directories.
sys.path.insert(0, str(Path(__file__).resolve().parent))
from pathguard import inside  # noqa: E402

REL_OPS = ["<", "<=", ">", ">=", "==", "!="]
REL = re.compile(r"(?<=\s)(<=|>=|==|!=|<|>)(?=\s)")
LOGIC = re.compile(r"(?<=\s)(&&|\|\|)(?=\s)")
INT = re.compile(r"(?<![\w.'])(\d+)([uU]?[lL]{0,2}z?|[uU]?z)?(?![\w.'])")
MINMAX = re.compile(r"\bstd::(ranges::)?(min|max)\b")
COND = re.compile(r"\b(if|while)\s*\(")
RET_BOOL = re.compile(r"\breturn (true|false);")
# A ninja target name; never a leading '-', which ninja would read as an option.
TARGET = re.compile(r"\w[\w.+/-]*")
# Statements whose deletion mostly changes logging or metrics wording, not behaviour.
QUIET = re.compile(r"\b(log|LOG|ulw_log|logger|trace|debug)\w*\s*[.(]|static_assert")


def mask(lines):
    """Blanks string and char literals and comments, keeping column positions."""
    out, in_block = [], False
    for line in lines:
        chars, i, n = list(line), 0, len(line)
        while i < n:
            if in_block:
                end = line.find("*/", i)
                stop = n if end < 0 else end + 2
                for j in range(i, stop):
                    chars[j] = " "
                in_block, i = end < 0, stop
                continue
            c = line[i]
            if line.startswith("//", i):
                for j in range(i, n):
                    chars[j] = " "
                break
            if line.startswith("/*", i):
                in_block = True
                continue
            if c in "\"'":
                raw = c == '"' and i > 0 and line[i - 1] == "R"
                if raw:
                    end = line.find(')"', i)
                    stop = n if end < 0 else end + 2
                else:
                    j = i + 1
                    while j < n and line[j] != c:
                        j += 2 if line[j] == "\\" else 1
                    stop = min(j + 1, n)
                for j in range(i + 1, stop - 1):
                    chars[j] = "_"
                i = stop
                continue
            i += 1
        out.append("".join(chars))
    return out


def balanced_end(text, start):
    depth = 0
    for i in range(start, len(text)):
        if text[i] == "(":
            depth += 1
        elif text[i] == ")":
            depth -= 1
            if depth == 0:
                return i
    return -1


def mutants_for(path, root):
    resolved = inside(path)
    rel = str(resolved.relative_to(root))
    src = read_source(resolved).split("\n")
    masked = mask(src)
    found = []

    def add(lineno, op, col, old, new, what):
        found.append({"id": f"{rel}:{lineno + 1}:{op}:{len([m for m in found if m['line'] == lineno + 1 and m['op'] == op])}",
                      "file": rel, "line": lineno + 1, "op": op, "col": col, "old": old,
                      "new": new, "what": what})

    for n, (line, m) in enumerate(zip(src, masked)):
        code = m.strip()
        if not code or code.startswith("#") or "static_assert" in code or "template" in code:
            continue
        for x in REL.finditer(m):
            for alt in REL_OPS:
                if alt != x.group(1):
                    add(n, "ror", x.start(), x.group(1), alt, f"{x.group(1)} -> {alt}")
        for x in LOGIC.finditer(m):
            alt = "||" if x.group(1) == "&&" else "&&"
            add(n, "lcr", x.start(), x.group(1), alt, f"{x.group(1)} -> {alt}")
        for x in COND.finditer(m):
            open_paren = x.end() - 1
            close = balanced_end(m, open_paren)
            if close > 0:
                inner = line[open_paren + 1:close]
                if inner.startswith("auto ") or ";" in inner:
                    continue
                add(n, "neg", open_paren, line[open_paren:close + 1], f"(!({inner}))",
                    f"negate {x.group(1)} condition")
        for x in MINMAX.finditer(m):
            alt = "max" if x.group(2) == "min" else "min"
            new = f"std::{x.group(1) or ''}{alt}"
            add(n, "minmax", x.start(), x.group(0), new, f"{x.group(0)} -> {new}")
        for x in RET_BOOL.finditer(m):
            alt = "false" if x.group(1) == "true" else "true"
            add(n, "ret", x.start(), x.group(0), f"return {alt};", f"return {x.group(1)} -> {alt}")
        if code == "return;":
            add(n, "ret", m.index("return;"), "return;", "", "delete early return")
        for x in INT.finditer(m):
            value = int(x.group(1))
            if len(x.group(1)) > 1 and x.group(1).startswith("0"):
                continue
            for delta in (1, -1):
                if value + delta < 0:
                    continue
                new = f"{value + delta}{x.group(2) or ''}"
                add(n, "const", x.start(), x.group(0), new, f"{x.group(0)} -> {new}")
        # An expression statement on one line: a call, an assignment or an increment.
        prev = masked[n - 1].strip() if n else ""
        is_cont = prev and not prev.endswith((";", "{", "}", ":")) and not prev.startswith("#")
        if (code.endswith(";") and not is_cont and not QUIET.search(code)
                and not code.endswith(("= delete;", "= default;"))
                and not re.match(r"(return|break|continue|case|default|using|namespace|"
                                 r"auto|const|static|constexpr|co_return|goto)\b", code)
                and code.count("(") == code.count(")")
                and (re.match(r"[\w.\->\[\]*]+(\(|\s*(=|\+=|-=|\|=|&=|<<=|>>=)\s|\+\+|--)", code)
                     or re.match(r"(\+\+|--)\w", code))
                and not re.match(r"[\w:<>,\s*&]+\s+\w+\s*(=|\(|\{|;)", code)):
            indent = len(line) - len(line.lstrip())
            add(n, "del", indent, line.strip(), "", f"delete `{line.strip()}`")
    return found


def read_source(path):
    # Bytes, not text mode, so that "\r\n" line endings survive the round trip.
    return inside(path).read_bytes().decode("utf-8")


def write_source(path, text):
    inside(path).write_bytes(text.encode("utf-8"))


def apply(path, mutant):
    text = read_source(path)
    lines = text.split("\n")
    line = lines[mutant["line"] - 1]
    col, old = mutant["col"], mutant["old"]
    if line[col:col + len(old)] != old:
        raise RuntimeError(f"{mutant['id']}: source changed, `{old}` not at column {col}")
    lines[mutant["line"] - 1] = line[:col] + mutant["new"] + line[col + len(old):]
    return text, "\n".join(lines)


class Stopped(Exception):
    """SIGTERM or SIGHUP, raised so that the file is restored on the way out."""


def on_stop_signal(signum, _frame):
    raise Stopped(signal.Signals(signum).name)


def restore(path, text, stop_signals):
    """Writes `text` back to `path` with the stop signals blocked, and returns the Stopped or
    KeyboardInterrupt that interrupted it, if any, for the caller to raise once it is safe.

    A signal handled before the block takes effect, or one already pending that fires as the
    block call returns, raises wherever the interpreter is; the write is then retried until it
    completes, so the source is never left mutated.
    """
    pending = None
    while True:
        try:
            signal.pthread_sigmask(signal.SIG_BLOCK, stop_signals)
            write_source(path, text)
            return pending
        except (Stopped, KeyboardInterrupt) as e:
            pending = e


def kill_group(p):
    try:
        os.killpg(p.pid, signal.SIGKILL)
    except ProcessLookupError:
        pass


def run(cmd, timeout, env=None, cwd=None):
    """Runs `cmd` in a process group of its own, which a timeout or a stop kills whole."""
    start = time.monotonic()
    p = subprocess.Popen(cmd, stdout=subprocess.PIPE, stderr=subprocess.STDOUT, env=env, cwd=cwd,
                         start_new_session=True)
    try:
        out, _ = p.communicate(timeout=timeout)
        return p.returncode, out.decode(errors="replace"), time.monotonic() - start
    except subprocess.TimeoutExpired:
        kill_group(p)
        out, _ = p.communicate()
        return None, out.decode(errors="replace"), time.monotonic() - start
    except BaseException:
        kill_group(p)
        p.wait()
        raise


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--files", nargs="+", required=True)
    ap.add_argument("--build-dir")
    ap.add_argument("--target")
    ap.add_argument("--sample", type=int, default=0, help="mutants to run, sampled evenly per file")
    ap.add_argument("--seed", type=int, default=1)
    ap.add_argument("--budget-s", type=float, default=0, help="stop starting mutants after this")
    ap.add_argument("--timeout", type=float, default=60, help="seconds per test run")
    ap.add_argument("--build-timeout", type=float, default=600)
    ap.add_argument("--ops", default="ror,lcr,neg,del,ret,const,minmax")
    ap.add_argument("--only", nargs="*", help="mutant ids to run")
    ap.add_argument("--lines", nargs="*", default=[],
                    help="restrict a file to line ranges: path:10-80,120-200")
    ap.add_argument("--list", action="store_true")
    ap.add_argument("--out")
    ap.add_argument("cmd", nargs=argparse.REMAINDER)
    a = ap.parse_args()
    root = Path.cwd().resolve()
    ops = set(a.ops.split(","))

    ranges = {}
    for spec in a.lines:
        path, _, spans = spec.rpartition(":")
        ranges[path] = [tuple(map(int, r.split("-"))) for r in spans.split(",")]

    def wanted_line(f, line):
        return f not in ranges or any(lo <= line <= hi for lo, hi in ranges[f])

    per_file = {f: [m for m in mutants_for(f, root) if m["op"] in ops and wanted_line(f, m["line"])]
                for f in a.files}
    if a.only:
        wanted = set(a.only)
        chosen = [m for ms in per_file.values() for m in ms if m["id"] in wanted]
        missing = wanted - {m["id"] for m in chosen}
        if missing:
            sys.exit(f"unknown mutant ids: {sorted(missing)}")
    elif a.sample:
        rng = random.Random(a.seed)
        pools = {f: rng.sample(ms, len(ms)) for f, ms in per_file.items()}
        chosen = []
        # Round-robin over the files, weighted by how many mutants each has.
        total = sum(len(ms) for ms in per_file.values()) or 1
        quota = {f: max(1, round(a.sample * len(ms) / total)) if ms else 0 for f, ms in per_file.items()}
        for f in a.files:
            chosen += pools[f][:quota[f]]
        # Interleaved, so a --budget-s that runs out still leaves every file sampled.
        rng.shuffle(chosen)
    else:
        chosen = [m for ms in per_file.values() for m in ms]
    if a.list:
        for m in chosen:
            print(f"{m['id']}\t{m['what']}")
        print(f"{len(chosen)} mutants of {sum(len(v) for v in per_file.values())}", file=sys.stderr)
        return 0
    cmd = a.cmd[1:] if a.cmd and a.cmd[0] == "--" else a.cmd
    if not (a.build_dir and a.target and cmd):
        sys.exit("--build-dir, --target and a test command are required unless --list")
    # A target is a name ninja knows, never one of its options.
    if not TARGET.fullmatch(a.target):
        sys.exit(f"--target {a.target!r} is not a ninja target name")
    # A test program named by path runs from inside the repository or a temporary directory;
    # a bare name is looked up on PATH, as before.
    if os.sep in cmd[0]:
        cmd = [str(inside(cmd[0]))] + cmd[1:]

    build = ["ninja", "-C", str(inside(a.build_dir)), "-j1", a.target]
    env = dict(os.environ, CCACHE_READONLY="1")
    rc, out, _ = run(build, a.build_timeout)
    if rc != 0:
        sys.exit(f"the unmutated target does not build:\n{out[-3000:]}")
    rc, out, base = run(cmd, a.timeout)
    if rc != 0:
        sys.exit(f"the unmutated tests do not pass:\n{out[-3000:]}")
    print(f"baseline: {base:.1f}s, {len(chosen)} mutants", file=sys.stderr, flush=True)

    for signum in (signal.SIGTERM, signal.SIGHUP):
        signal.signal(signum, on_stop_signal)
    stop_signals = {signal.SIGINT, signal.SIGTERM, signal.SIGHUP}
    out_f = open(inside(a.out), "a", encoding="utf-8") if a.out else None
    counts = {}
    started = time.monotonic()
    # The file and original text of the mutant in progress, which the outer `finally` writes
    # again in case a stop signal landed before the per-mutant restore could start.
    in_progress = None
    try:
        for i, m in enumerate(chosen):
            if a.budget_s and time.monotonic() - started > a.budget_s:
                print("budget spent", file=sys.stderr)
                break
            original, mutated = apply(m["file"], m)
            in_progress = (m["file"], original)
            try:
                write_source(m["file"], mutated)
                rc, bout, bt = run(build, a.build_timeout, env=env)
                if rc != 0:
                    status, tt, tail = "unbuildable", 0.0, ""
                else:
                    rc, tout, tt = run(cmd, a.timeout)
                    status = "timeout" if rc is None else ("survived" if rc == 0 else "killed")
                    failed = re.findall(r"\[  FAILED  \] (\S+)", tout)
                    tail = ",".join(sorted(set(failed))[:5]) if failed else tout[-300:]
            finally:
                # No stop signal may cut the restore short.
                pending = restore(m["file"], original, stop_signals)
                in_progress = None
                signal.pthread_sigmask(signal.SIG_UNBLOCK, stop_signals)
                if pending:
                    raise pending
            counts[status] = counts.get(status, 0) + 1
            rec = dict(m, status=status, build_s=round(bt, 1), test_s=round(tt, 1),
                       evidence=tail if status == "killed" else "")
            print(f"[{i + 1}/{len(chosen)}] {status:11} {m['id']}  {m['what']}", flush=True)
            if out_f:
                out_f.write(json.dumps(rec) + "\n")
                out_f.flush()
    finally:
        pending = restore(*in_progress, stop_signals) if in_progress else None
        if out_f:
            out_f.close()
        # Unblocked before the rebuild: blocked, neither this process nor the children that
        # inherit the mask would answer Ctrl-C or SIGTERM until --build-timeout. The source is
        # restored, so a stop signal may now cut the rebuild short.
        signal.pthread_sigmask(signal.SIG_UNBLOCK, stop_signals)
        run(build, a.build_timeout)
        if pending:
            raise pending
    scored = counts.get("killed", 0) + counts.get("timeout", 0) + counts.get("survived", 0)
    score = (counts.get("killed", 0) + counts.get("timeout", 0)) / scored if scored else 0
    print(f"summary: {counts} score={score:.1%}", flush=True)
    return 0


if __name__ == "__main__":
    try:
        sys.exit(main())
    except Stopped as stopped:
        sys.exit(f"stopped by {stopped}; the source is restored")
