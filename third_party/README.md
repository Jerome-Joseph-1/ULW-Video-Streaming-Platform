# Vendored dependencies

Each tarball is consumed by `cmake/Dependencies.cmake` with a `URL_HASH`. A hash mismatch
fails the configure step; never update a hash to match a download.

| File | Upstream | SHA-256 |
|---|---|---|
| `llhttp-9.2.1.tar.gz` | https://github.com/nodejs/llhttp/archive/refs/tags/release/v9.2.1.tar.gz | `3c163891446e529604b590f9ad097b2e98b5ef7e4d3ddcf1cf98b62ca668f23e` |
| `googletest-1.15.2.tar.gz` | https://github.com/google/googletest/archive/refs/tags/v1.15.2.tar.gz | `7b42b4d6ed48810c5362c265a17faebe90dc2373c885e5216439d37927f02926` |
| `srt-1.5.4.tar.gz` | https://github.com/Haivision/srt (tag `v1.5.4`, commit `a8c6b65520f814c5bd8f801be48c33ceece7c4a6`) | `d0a8b600fe1b4eaaf6277530e3cfc8f15b8ce4035f16af4a5eb5d4b123640cdd` |

All are byte-identical to `git archive --format=tar.gz` of the tag with `gzip -cn -6`, so
they can be reproduced from a clone when the archive endpoint is unavailable:

```sh
git -c tar.tar.gz.command="gzip -cn -6" archive --format=tar.gz \
    --prefix=googletest-1.15.2/ v1.15.2 > googletest-1.15.2.tar.gz
```
