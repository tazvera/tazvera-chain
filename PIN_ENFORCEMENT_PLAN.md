# Genesis pin enforcement — plan, not protection

**Status: not enforced anywhere today.** This document exists so that nobody reads the pin code and concludes
that genesis authentication is solved.

## What is true today (v8.11)

- `Chain::genesis_commitment()` commits to the genesis account list, the chain ID, the protocol version, the
  parameters and block 0's hash. It is written into every exported chain file.
- The loader recomputes it and refuses a file whose commitment does not match — **tamper detection**.
- `LoadOptions::expected_genesis_commitment` lets a caller compare against a value published out of band.
  **No caller sets it.**
- `LoadOptions::require_genesis_pin` refuses a load that has no pin. **No caller sets it.**
- There is no node, no CLI and no config file, so nothing can set either option.

Consequence: **any internally consistent chain loads.** An attacker who builds their own genesis and hands
you the file produces a chain in which every hash agrees. The pin is the only defence against that, and it is
not in use.

## What a pin does and does not prove

It proves the genesis you loaded is the genesis you meant to load. It says nothing about whether that chain is
honest, well run, or worth joining. Authenticity of the *operator* is a different problem (published release
artefacts, signed tags, reproducible builds).

## v9 — the node sets it

- `taznode` takes `--expect-genesis <64 hex>` or reads `expect_genesis` from its config file.
- The node sets `require_genesis_pin = true` unconditionally. **A node with no pin refuses to start.**
- Tests: no pin → refuse to start; wrong pin → refuse; matching pin → start; a node with the wrong pin never
  exchanges a block with a node that has the right one (the handshake already compares the commitment).
- The genesis commitment is printed in the node's startup log, so an operator can compare it with the release
  notes without reading a file.

## v10 — the library defaults to requiring a pin

- `require_genesis_pin` becomes `true` by default.
- The option is inverted into an explicit escape hatch (`allow_unpinned_load`) that logs a warning.
- Anything that still loads without a pin is a test or a migration tool, and says so in its own code.

## Rule for every document until then

The phrase "production security" does not appear in this repository until there is a node, independent
validators and an external audit. The pin is a step towards being able to say it, not evidence for it.

## Implementation plan for the node (v9) — not implemented yet

Nothing below exists in the code today. It is written down so the work can be reviewed before it is built, and
so nobody mistakes the current options for protection.

**CLI / config surface**

```
taznode --genesis <file> --expect-genesis <64 hex>   # or: expect_genesis = "<64 hex>" in taznode.toml
```

**Loader behaviour**

1. `taznode` always sets `LoadOptions::require_genesis_pin = true`.
2. It passes the pinned value in `LoadOptions::expected_genesis_commitment`.
3. Missing `--expect-genesis` is a startup error, not a warning: a node that cannot name its chain must not run.
4. The commitment is printed once at startup, next to the chain ID and the protocol version, so an operator can
   compare it with the release notes without opening a file.

**Required tests, in the order they should be written**

| # | Test | What it must prove |
|---|---|---|
| 1 | `node_refuses_without_pin` | no `--expect-genesis` → exit code != 0, message names the flag, no file is loaded |
| 2 | `node_refuses_wrong_pin` | a valid file plus a wrong pin → refused, and the error says which chain it expected |
| 3 | `node_accepts_matching_pin` | the same file plus the right pin → loads, height and state root as expected |
| 4 | `node_wrong_pin_never_exchanges_blocks` | two nodes, one with a wrong pin: the handshake compares the commitment and the connection is dropped before any block is sent |
| 5 | `node_prints_commitment_at_startup` | the log line contains the same 64 hex characters as `Chain::genesis_commitment()` |
| 6 | `library_default_unchanged` | a library caller that sets nothing behaves exactly as today (this is the compatibility test) |

**Order of work**

1. Add the flag and the config key, with `require_genesis_pin` still off by default.
2. Write tests 1–3 against the flag (they can run without a network).
3. Add the startup log line (test 5).
4. Only then the P2P handshake comparison (test 4).
5. v10 flips the library default; that release carries test 6 and a migration note.

**What must not happen**

- The pin must not become mandatory in the library before the node exists, or every existing caller breaks for
  no security gain.
- No document may call the pin "production security" before there is a node, independent validators and an
  external audit.
