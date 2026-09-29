# End-to-end encryption

> **Draft: changes until milestone M22 merges.** Nothing on `main` serves key directories or MLS
> yet. This page states the model only.

Private 1:1 and small-group chat is end-to-end encrypted with MLS (ADR-0016); a 1:1 chat is an
MLS group of two. All keys are generated and held by client devices; the server never holds or
sees them. The server keeps a directory of devices and their single-use KeyPackages, hands one
out per fetch (and signals the owner to replenish when none are left), and carries MLS
handshake and application messages as opaque chat bodies. The room's total order (`seq`) is
what orders Commits: members apply the first valid Commit for an epoch in `seq` order and
discard the rest. Multi-device key management is a known gap.
