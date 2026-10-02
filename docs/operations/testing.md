# Mutation testing

`tools/mutate.py` checks whether a unit suite notices small changes to the code it covers. It
applies one mutation at a time to a source file, rebuilds one test target, runs the suite, and
restores the file:

- relational operators (`<` `<=` `>` `>=` `==` `!=`) swapped for each of the others;
- `&&` and `||` swapped;
- the condition of an `if` or `while` negated;
- an expression statement deleted, `return true/false` flipped, an early `return;` deleted;
- an integer literal moved by one either way;
- `std::min` and `std::max` swapped.

A mutant is *killed* when the suite fails, *timeout* when it outlasts `--timeout`, *survived*
when it passes, and *unbuildable* (left out of the score) when `-Werror` refuses it. Each mutant
has an id, `file:line:operator:index`, so a surviving one can be run again after a test is added.

```
cmake --preset ci -B build/mut -DCMAKE_CXX_COMPILER_LAUNCHER=ccache
cmake --build build/mut --target http_unit_tests
nice -n 19 tools/mutate.py --build-dir build/mut --target http_unit_tests \
    --files http/src/request_parser.cpp --sample 200 --budget-s 1500 --timeout 30 \
    --out /tmp/http.jsonl -- build/mut/tests/http_unit_tests --gtest_brief=1 --gtest_fail_fast
tools/mutate.py --list --files http/src/request_parser.cpp        # the mutants, not run
tools/mutate.py ... --only http/src/request_parser.cpp:202:del:0  # chosen mutants again
```

The test command after `--` is either a test binary inside the repository or the build tree,
given only googletest flags (`--gtest_*`, passed to it as the `GTEST_*` environment variables
googletest reads in their place; `--gtest_list_tests` and `--gtest_output` are refused), or
`ctest`, run in the build tree, with exactly these options, none of which takes a value:
`--output-on-failure`, `--stop-on-failure`, `--no-tests=error`, `--schedule-random`, `-Q`,
`--quiet`, `-V` and `--verbose`. Anything else is refused with exit 2, as is a `--files` path
outside the directory it runs from.

`--lines path:10-80,120-200` restricts a file to the functions under study, and `--sample`
spreads the mutants over the files in proportion to how many each has; the sample is the mutants
whose SHA-256 of seed and id sorts first, so a `--seed` repeats it exactly. Each mutant rebuilds
one object and relinks one binary; mutants build with `CCACHE_READONLY`, so they do not fill the
cache, and the restored source hits it. Run it from its own worktree: it edits sources in place.
It restores the file when stopped by SIGINT, SIGTERM or SIGHUP, and kills a timed-out build or
test with its whole process group; after a SIGKILL, `git checkout` the file it was mutating.

A survivor is either a missing test or an equivalent mutant, one no test can tell apart:
`x == npos` read as `x >= npos`, `n == 0` as `n <= 0` for an unsigned `n`, a buffer grown by
another factor, a bound the code above it already enforces. The first kind gets a test.

## First run

Sampled mutants, about 25 minutes of mutant run time per suite, before the tests that followed
it:

| Code | Suite | Run | Killed | Timeout | Survived | Score |
|---|---|---|---|---|---|---|
| `http/src/request_parser.cpp` | `http_unit_tests` | 189 | 138 | 9 | 42 | 77.8% |
| `codec/ws/src/*.cpp` | `ws_unit_tests` | 180 | 153 | 2 | 25 | 86.1% |
| gateway admission, limits, timeouts | `gateway_tests` | 40 | 18 | 4 | 18 | 55.0% |
| chat ordering, kept messages, sessions | `chat_unit_tests` | 67 | 47 | 0 | 20 | 70.1% |
| `infra/auth/src/*.cpp` | `auth_unit_tests` | 211 | 150 | 4 | 57 | 73.0% |
| `core/src/*.cpp` | `core_unit_tests` | 239 | 179 | 2 | 58 | 75.7% |
