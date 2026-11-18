# Genesis pin enforcement — plan, not protection

**Status: not enforced anywhere today.** This document exists so that nobody reads the pin code and concludes
that genesis authentication is solved.

## What is true today (v8.9)

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
