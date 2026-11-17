# CORE V8.2 Security Design

**Status:** design + implementation on branch `core-v8.2-security` (not production).  
**Protocol version:** **2** (breaking for recovery/veto and state-root encoding of `veto_postponed`).

## A. Genesis integrity
Problem: zero-balance genesis accounts are not stored in state, so neither the state root nor any block hash covered their addresses. A "commitment" that is only a field in the same file, checked against the same file's balances, proves nothing (an attacker edits both).
Decision (implemented): `commitment = SHA-256("TAZVERA-GENESIS-V2" || chain_id || u32 protocol || sorted (address, balance_be64) x2)`, stored as block 0's `previous_hash`. Block 0's hash, and every later `previous_hash`, then depend on it. Loader requires `block0.previous_hash == commitment` and `block0.hash == rebuilt hash`. Old v1 files are refused. Residual: a genesis-only file needs out-of-band comparison.

## B. Guardian / veto lockout
Attack: stolen owner key schedules its own veto key (500-block delay); late guardians then lose forever (`test_thief_adds_veto_late`).
Decision (implemented, protocol 2):
1. Discard rule: a pending guardian/veto change due while `recovery.active` is dropped (state.cpp `begin_block`).
2. R2: a live veto postpones an active recovery once by `VETO_POSTPONE_BLOCKS` = 300; the second veto fails (`veto_postponed` in state root).
Rejected: R1 only (late guardians still lose); permanent cancel (original problem); counter-based limits on owner cancel (would remove the owner's ability to stop malicious guardians; listed as a known limitation).
Fork risk: nodes using other rules diverge, hence PROTOCOL_VERSION 2.

## C. Persistence

Already present / tightened in v8.x: atomic tmp+rename, strict JSON keys, decimal strings, hex, replay via `accept_block`.  
v8.2: required `genesis_commitment`, protocol 2 gate.

## D. Not production-ready

No P2P, no consensus, no audited crypto review, demo keys only, fee economics unresolved, parameters are placeholders.

## E. Tests expected
- Recovery: thief late veto → postpone → funds still move.
- Persist: commitment mismatch / missing commitment fail; round-trip still works.
