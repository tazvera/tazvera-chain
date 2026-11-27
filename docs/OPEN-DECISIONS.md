# CHAIN REQUIREMENTS — Corrections found during code review
# Corrections found while checking the master checklist

## 1. Chain ID and genesis commitment

The original list said that chain_id is not bound to genesis. This is not correct: genesis_commitment_of(...) includes the fixed CHAIN_ID, and the loader checks the file's chain_id and pinned genesis commitment. Transaction/hello identity also includes the chain ID/genesis commitment.

Correct status: chain_id is bound to the genesis commitment, but it is currently compile-time fixed to tazvera-local-prototype-v1; there is no option to configure it for a separate network. Therefore, the checklist marks this as partial.

## 2. Peer addressing

With --listen-on <IPv4>, a node can listen for inbound connections on the specified address, including 0.0.0.0. However, --peer currently accepts only a port and connects to 127.0.0.1 through connect_loopback(). Therefore, external inbound listening exists, but complete two-way remote peer addressing does not.

## 3. API and Explorer

The API is read-only and binds to 127.0.0.1; node help says GET-only. POST /tx does not exist. The Explorer verifies a selected JSON file; it is not a live API explorer.

## Scope

This correction log is kept in docs/ so that this PR changes only docs/ paths. The root-level OPEN-DECISIONS.md was not changed in this docs-only PR.
