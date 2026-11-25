# F5 — Open decisions and explicit scope

## Fixed behavior for this PR

- When two peers have the same genesis and height but disagree on history, the current implementation can only test whether the peer's advertised tip attaches directly to the local chain after removing the local tip.
- If that attachment fails, the node must reject the sync, emit a clear refusal message, and return a non-zero result. It must not report the peer as successfully served or the sync round as successful.
- This PR does not attempt a common-ancestor search or a multi-block reorganization.

## Deferred decisions

1. **Common-ancestor reorganization:** a separate PR must define and test how to locate the highest common ancestor, validate every replacement block in order, and commit the replacement atomically. Until then, deeper forks are refused.
2. **Fork choice and finality:** the existing height/tip-hash selection rule is only a deterministic tie-break, not consensus or finality. It must not be described as consensus.
3. **Peer retry policy:** whether a node should automatically retry a refused deep fork, and with what backoff/limit, remains a separate networking decision. This PR only guarantees that the failed sync is not reported as success.
4. **Unequal-height divergent histories:** this PR's regression tests cover equal-height deep forks only. A separate test/review must define behavior when a longer peer chain diverges before the local tip; no common-ancestor reorg is introduced here.

## Validation boundary

- Regression cases: divergence at block 1, with both chains at heights 2, 3, and 10.
- Expected result: non-zero exit from the connecting node, explicit "unrepaired deep fork" refusal, and neither chain file modified by the failed exchange.
- No changes to networking protocol, consensus, persistence format, or common-ancestor reorganization.


# F11 — Genesis supply bound

## Fixed behavior for this PR

- Genesis creation rejects when the sum of the two genesis balances exceeds `std::numeric_limits<Amount>::max()` (the uint64 maximum).
- The check uses subtraction (`second_balance > max - first_balance`) so the validation itself cannot overflow.
- The check runs before any chain or producer state is mutated, for both producer-free and producer-enabled genesis creation.
- A total supply exactly equal to uint64 maximum remains valid.
- The existing receiver-overflow regression now uses a legal maximum-supply genesis; the state-level overflow guard remains defense in depth.

## Deferred decisions

1. **Protocol supply cap:** this PR treats uint64 maximum as the upper bound. Whether TAZVERA should adopt a lower economic/protocol supply cap is a separate protocol decision.
2. **Genesis account count:** the current API accepts exactly two genesis accounts. If genesis later supports a variable number of allocations, the same checked-sum invariant must cover every allocation before committing any state.
3. **Commitment helper semantics:** `genesis_commitment_of(...)` remains a deterministic hash helper, not a validation API. Callers that create a chain must use the checked `Chain::init_genesis` path; whether to expose a separate validated genesis builder is deferred.

## Validation boundary

- Overflow by one (uint64 max + 1) is rejected for both genesis overloads.
- Exact uint64 maximum is accepted for both overloads.
- Rejection leaves chain state and producer configuration unchanged and permits a subsequent valid genesis.
